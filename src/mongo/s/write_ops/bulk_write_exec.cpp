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
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands/bulk_write_common.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/database_name.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/bulk_write_command_modifier.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace bulk_write_exec {
namespace {

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

bool isVerboseWc(const BSONObj& wc) {
    BSONElement wElem = wc["w"];
    return !wElem.isNumber() || wElem.Number() != 0;
}

/**
 * Send and process the child batches. Each child batch is targeted at a unique shard: therefore one
 * shard will have only one batch incoming.
 */
void executeChildBatches(OperationContext* opCtx,
                         const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                         TargetedBatchMap& childBatches,
                         BulkWriteOp& bulkWriteOp,
                         stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace,
                         boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (auto& childBatch : childBatches) {
        auto request = [&]() {
            auto bulkReq = bulkWriteOp.buildBulkCommandRequest(
                targeters, *childBatch.second, allowShardKeyUpdatesWithoutFullShardKeyInQuery);

            // Transform the request into a sendable BSON.
            BSONObjBuilder builder;
            bulkReq.serialize(BSONObj(), &builder);

            logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);

            // Per-operation write concern is not supported in transactions.
            if (!TransactionRouter::get(opCtx)) {
                auto wc = opCtx->getWriteConcern().toBSON();
                if (isVerboseWc(wc)) {
                    builder.append(WriteConcernOptions::kWriteConcernField, wc);
                } else {
                    // Mongos needs to send to the shard with w > 0 so it will be able to see the
                    // writeErrors
                    builder.append(WriteConcernOptions::kWriteConcernField,
                                   upgradeWriteConcern(wc));
                }
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

    // The BulkWriteOp may be marked finished early if we are in a transaction and encounter an
    // error, which aborts the transaction. In those cases, we do not bother waiting for any
    // outstanding responses from shards.
    while (!ars.done() && !bulkWriteOp.isFinished()) {
        // Block until a response is available.
        auto response = ars.next();

        TargetedWriteBatch* writeBatch = childBatches.find(response.shardId)->second.get();
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

void fillOKInsertReplies(BulkWriteReplyInfo& replyInfo, int size) {
    replyInfo.replyItems.reserve(size);
    for (int i = 0; i < size; ++i) {
        BulkWriteReplyItem reply;
        reply.setN(1);
        reply.setOk(1);
        reply.setIdx(i);
        replyInfo.replyItems.push_back(reply);
    }
}

BulkWriteReplyInfo processFLEResponse(const BulkWriteCRUDOp::OpType& firstOpType,
                                      const BatchedCommandResponse& response) {
    BulkWriteReplyInfo replyInfo;
    if (response.toStatus().isOK()) {
        if (firstOpType == BulkWriteCRUDOp::kInsert) {
            fillOKInsertReplies(replyInfo, response.getN());
        } else {
            BulkWriteReplyItem reply;
            reply.setN(response.getN());
            if (firstOpType == BulkWriteCRUDOp::kUpdate) {
                if (response.isUpsertDetailsSet()) {
                    std::vector<BatchedUpsertDetail*> upsertDetails = response.getUpsertDetails();
                    invariant(upsertDetails.size() == 1);
                    // BulkWrite needs only _id, not index.
                    reply.setUpserted(
                        IDLAnyTypeOwned(upsertDetails[0]->getUpsertedID().firstElement()));
                }

                reply.setNModified(response.getNModified());
            }
            reply.setOk(1);
            reply.setIdx(0);
            replyInfo.replyItems.push_back(reply);
        }
    } else {
        if (response.isErrDetailsSet()) {
            const auto& errDetails = response.getErrDetails();
            if (firstOpType == BulkWriteCRUDOp::kInsert) {
                fillOKInsertReplies(replyInfo, response.getN() + errDetails.size());
                for (const auto& err : errDetails) {
                    int32_t idx = err.getIndex();
                    replyInfo.replyItems[idx].setN(0);
                    replyInfo.replyItems[idx].setOk(0);
                    replyInfo.replyItems[idx].setStatus(err.getStatus());
                }
            } else {
                invariant(errDetails.size() == 1 && response.getN() == 0);
                BulkWriteReplyItem reply(0, errDetails[0].getStatus());
                reply.setN(0);
                if (firstOpType == BulkWriteCRUDOp::kUpdate) {
                    reply.setNModified(0);
                }
                replyInfo.replyItems.push_back(reply);
            }
            replyInfo.numErrors += errDetails.size();
        } else {
            // response.toStatus() is not OK but there is no errDetails so the
            // top level status should be not OK instead. Raising an exception.
            uassertStatusOK(response.getTopLevelStatus());
            MONGO_UNREACHABLE;
        }
        // TODO (SERVER-81280): Handle write concern errors.
    }
    return replyInfo;
}

}  // namespace

std::pair<FLEBatchResult, BulkWriteReplyInfo> attemptExecuteFLE(
    OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest) {
    const auto& ops = clientRequest.getOps();
    BulkWriteCRUDOp firstOp(ops[0]);
    auto firstOpType = firstOp.getType();
    try {
        BatchedCommandResponse response;
        FLEBatchResult fleResult;

        if (firstOpType == BulkWriteCRUDOp::kInsert) {
            std::vector<mongo::BSONObj> documents;
            documents.reserve(ops.size());
            for (const auto& opVariant : ops) {
                BulkWriteCRUDOp op(opVariant);
                uassert(ErrorCodes::InvalidOptions,
                        "BulkWrite with Queryable Encryption and multiple operations supports only "
                        "insert.",
                        op.getType() == BulkWriteCRUDOp::kInsert);
                documents.push_back(op.getInsert()->getDocument());
            }

            write_ops::InsertCommandRequest insertOp =
                bulk_write_common::makeInsertCommandRequestForFLE(
                    documents, clientRequest, clientRequest.getNsInfo()[0]);

            BatchedCommandRequest fleRequest(insertOp);
            fleResult = processFLEBatch(
                opCtx, fleRequest, nullptr /* stats */, &response, {} /*targetEpoch*/);
        } else if (firstOpType == BulkWriteCRUDOp::kUpdate) {
            uassert(ErrorCodes::InvalidOptions,
                    "BulkWrite update with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);

            write_ops::UpdateCommandRequest updateCommand =
                bulk_write_common::makeUpdateCommandRequestFromUpdateOp(
                    firstOp.getUpdate(), clientRequest, /*currentOpIdx=*/0);

            BatchedCommandRequest fleRequest(updateCommand);
            fleResult = processFLEBatch(
                opCtx, fleRequest, nullptr /* stats */, &response, {} /*targetEpoch*/);
        } else {
            uassert(ErrorCodes::InvalidOptions,
                    "BulkWrite delete with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);

            write_ops::DeleteCommandRequest deleteCommand =
                bulk_write_common::makeDeleteCommandRequestForFLE(
                    opCtx, firstOp.getDelete(), clientRequest, clientRequest.getNsInfo()[0]);

            BatchedCommandRequest fleRequest(deleteCommand);
            fleResult = processFLEBatch(
                opCtx, fleRequest, nullptr /* stats */, &response, {} /*targetEpoch*/);
        }

        if (fleResult == FLEBatchResult::kNotProcessed) {
            return {FLEBatchResult::kNotProcessed, BulkWriteReplyInfo()};
        }

        BulkWriteReplyInfo replyInfo = processFLEResponse(firstOpType, response);
        return {FLEBatchResult::kProcessed, std::move(replyInfo)};
    } catch (const DBException& ex) {
        LOGV2_WARNING(7749700,
                      "Failed to process bulkWrite with Queryable Encryption",
                      "error"_attr = redact(ex));
        // If Queryable encryption adds support for update with multi: true, we might have to update
        // the way we make replies here to handle SERVER-15292 correctly.
        BulkWriteReplyInfo replyInfo;
        BulkWriteReplyItem reply(0, ex.toStatus());
        reply.setN(0);
        if (firstOpType == BulkWriteCRUDOp::kUpdate) {
            reply.setNModified(0);
        }

        replyInfo.replyItems.push_back(reply);
        replyInfo.numErrors = 1;
        return {FLEBatchResult::kProcessed, std::move(replyInfo)};
    }
}

void executeRetryableTimeseriesUpdate(OperationContext* opCtx,
                                      TargetedBatchMap& childBatches,
                                      BulkWriteOp& bulkWriteOp) {
    invariant(!childBatches.empty());
    // Get the index of the targeted operation in the client bulkWrite request.
    auto opIdx = childBatches.begin()->second->getWrites()[0]->writeOpRef.first;

    // Construct a single-op update request based on the update operation at opIdx.
    auto& bulkWriteReq = bulkWriteOp.getClientRequest();

    BulkWriteCommandRequest singleUpdateRequest;
    singleUpdateRequest.setOps({bulkWriteReq.getOps()[opIdx]});
    singleUpdateRequest.setNsInfo(bulkWriteReq.getNsInfo());
    singleUpdateRequest.setBypassDocumentValidation(bulkWriteReq.getBypassDocumentValidation());
    singleUpdateRequest.setLet(bulkWriteReq.getLet());
    singleUpdateRequest.setStmtId(bulk_write_common::getStatementId(bulkWriteReq, opIdx));
    singleUpdateRequest.setDbName(DatabaseName::kAdmin);

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

    bulkWriteOp.handleErrorsForRetryableTimeseriesUpdate(swResult, childBatches.begin()->first);

    // We should get back just one reply item for the single update we are running.
    const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
    tassert(7934203, "unexpected reply for retryable timeseries update", replyItems.size() == 1);
    LOGV2_DEBUG(7934204,
                4,
                "Processing bulk write response for retryable timeseries update",
                "opIdx"_attr = opIdx,
                "singleUpdateRequest"_attr = redact(singleUpdateRequest.toBSON({})),
                "replyItem"_attr = replyItems[0]);
    bulkWriteOp.noteWriteOpFinalResponse(
        opIdx, replyItems[0], bulkWriteResponse.getRetriedStmtIds());
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

    // If there is only 1 targetable shard, we can skip using the two phase write protocol.
    if (targeter->getNShardsOwningChunks() == 1) {
        executeChildBatches(opCtx,
                            targeters,
                            childBatches,
                            bulkWriteOp,
                            errorsPerNamespace,
                            allowShardKeyUpdatesWithoutFullShardKeyInQuery);
    } else {
        // Execute the two phase write protocol for writes that cannot directly target a shard.

        // TODO (SERVER-77871): Support shard key metrics sampling.
        auto cmdObj = bulkWriteOp
                          .buildBulkCommandRequest(targeters,
                                                   *childBatches.begin()->second.get(),
                                                   allowShardKeyUpdatesWithoutFullShardKeyInQuery)
                          .toBSON({});

        auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
            opCtx, targeter->getNS(), std::move(cmdObj));

        BulkWriteCommandReply bulkWriteResponse;
        Status responseStatus = swRes.getStatus();
        if (swRes.isOK()) {
            std::string errMsg;
            if (swRes.getValue().getResponse().isEmpty()) {
                // When we get an empty response, it means that the predicate didn't match anything
                // and no write was done. So we can just set a trivial ok response.
                bulkWriteResponse.setCursor(BulkWriteCommandResponseCursor(
                    0,  // cursorId
                    std::vector<mongo::BulkWriteReplyItem>{BulkWriteReplyItem(0)},
                    NamespaceString::makeBulkWriteNSS(boost::none)));
            } else {
                try {
                    bulkWriteResponse = BulkWriteCommandReply::parse(
                        IDLParserContext("BulkWriteCommandReplyForWriteWithoutShardKey"),
                        swRes.getValue().getResponse());
                } catch (const DBException& ex) {
                    responseStatus = ex.toStatus().withContext(
                        "Failed to parse response from writes without shard key");
                }
            }
        }

        if (!responseStatus.isOK()) {
            // Set an error for the operation.
            bulkWriteResponse.setCursor(BulkWriteCommandResponseCursor(
                0,  // cursorId
                std::vector<mongo::BulkWriteReplyItem>{BulkWriteReplyItem(0, responseStatus)},
                NamespaceString::makeBulkWriteNSS(boost::none)));
        }

        // We should get back just one reply item for the single update we are running.
        const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
        tassert(7298301,
                "unexpected bulkWrite reply for writes without shard key",
                replyItems.size() == 1);
        LOGV2_DEBUG(7298302,
                    4,
                    "Processing bulk write response for writes without shard key",
                    "opIdx"_attr = opIdx,
                    "replyItem"_attr = replyItems[0]);
        bulkWriteOp.noteWriteOpFinalResponse(
            opIdx, replyItems[0], bulkWriteResponse.getRetriedStmtIds());
    }
}

void BulkWriteOp::handleErrorsForRetryableTimeseriesUpdate(
    StatusWith<mongo::txn_api::CommitResult>& swResult, const ShardId& shardId) {
    Status responseStatus = Status::OK();
    if (!swResult.isOK()) {
        responseStatus = swResult.getStatus();
    } else {
        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
        if (auto wcError = swResult.getValue().wcError; !wcError.toStatus().isOK()) {
            saveWriteConcernError(shardId, wcError);
        }
    }
    // TODO (SERVER-81006): Handle local error correctly.
    uassertStatusOK(responseStatus);
}

BulkWriteReplyInfo execute(OperationContext* opCtx,
                           const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                           const BulkWriteCommandRequest& clientRequest) {
    LOGV2_DEBUG(7263700,
                4,
                "Starting execution of a bulkWrite",
                "clientRequest"_attr = redact(clientRequest.toBSON({})));

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
            if (targetStatus.getValue() == WriteType::TimeseriesRetryableUpdate) {
                executeRetryableTimeseriesUpdate(opCtx, childBatches, bulkWriteOp);
            } else if (targetStatus.getValue() == WriteType::WithoutShardKeyOrId) {
                executeWriteWithoutShardKey(
                    opCtx, targeters, childBatches, bulkWriteOp, errorsPerNamespace);
            } else {
                // Send the child batches and wait for responses.
                executeChildBatches(opCtx,
                                    targeters,
                                    childBatches,
                                    bulkWriteOp,
                                    errorsPerNamespace,
                                    /*allowShardKeyUpdatesWithoutFullShardKeyInQuery=*/boost::none);
            }

            // If we saw any staleness errors, tell the targeters to invalidate their cache
            // so that they may be refreshed.
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
    }

    LOGV2_DEBUG(7263701, 4, "Finished execution of bulkWrite");
    return bulkWriteOp.generateReplyInfo();
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

StatusWith<WriteType> BulkWriteOp::target(const std::vector<std::unique_ptr<NSTargeter>>& targeters,
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
            // If retryable writes are used, MongoS needs to send an additional array of stmtId(s)
            // corresponding to the statements that got routed to each individual shard, so they
            // need to be accounted in the potential request size so it does not exceed the max BSON
            // size.
            const int writeSizeBytes = writeOp.getWriteItem().getSizeForBulkWriteBytes() +
                write_ops::kWriteCommandBSONArrayPerElementOverheadBytes +
                (_txnNum ? write_ops::kStmtIdSize +
                         write_ops::kWriteCommandBSONArrayPerElementOverheadBytes
                         : 0);
            return writeSizeBytes;
        },
        getBaseChildBatchCommandSizeEstimate(),
        targetedBatches);
}

BulkWriteCommandRequest BulkWriteOp::buildBulkCommandRequest(
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    const TargetedWriteBatch& targetedBatch,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    BulkWriteCommandRequest request;

    // A single bulk command request batch may contain operations of different
    // types, i.e. they may be inserts, updates or deletes.
    std::vector<
        stdx::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>>
        ops;
    std::vector<NamespaceInfoEntry> nsInfo = _clientRequest.getNsInfo();

    std::vector<int> stmtIds;
    if (_isRetryableWrite)
        stmtIds.reserve(targetedBatch.getNumOps());

    for (const auto& targetedWrite : targetedBatch.getWrites()) {
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
        auto nsIdx = bulkWriteOp.getNsInfoIdx();
        auto& nsInfoEntry = nsInfo.at(nsIdx);
        auto& targeter = targeters.at(nsIdx);

        auto isClientRequestOnTimeseriesBucketCollection =
            nsInfoEntry.getNs().isTimeseriesBucketsCollection();
        if (targeter->isShardedTimeSeriesBucketsNamespace() &&
            !isClientRequestOnTimeseriesBucketCollection) {
            // For sharded timeseries collections, only the bucket collections are sharded. This
            // sets the namespace to the namespace of the sharded bucket collection.
            nsInfoEntry.setNs(targeter->getNS());
            nsInfoEntry.setIsTimeseriesNamespace(true);
        }

        // If we are using the two phase write protocol introduced in PM-1632, we allow shard key
        // updates without specifying the full shard key in the query if we execute the update in a
        // retryable write/transaction.
        if (bulkWriteOp.getType() == BulkWriteCRUDOp::OpType::kUpdate &&
            allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
            auto mutableUpdateOp = stdx::get_if<BulkWriteUpdateOp>(&ops.back());
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
    request.setNsInfo(nsInfo);

    // It isn't necessary to copy the cursor options over, because the cursor options
    // are for use in the interaction between the mongos and the client and not
    // internally between the mongos and the mongods.
    request.setOrdered(_clientRequest.getOrdered());
    request.setBypassDocumentValidation(_clientRequest.getBypassDocumentValidation());
    request.setLet(_clientRequest.getLet());

    if (_isRetryableWrite) {
        request.setStmtIds(stmtIds);
    }

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

void BulkWriteOp::noteErrorForRemainingWrites(const Status& status) {
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

/**
 * Checks if an error reply has the TransientTransactionError label. We use this in cases where we
 * want to defer to whether a shard attached the label to an error it gave us.
 */
bool hasTransientTransactionErrorLabel(const ErrorReply& reply) {
    auto errorLabels = reply.getErrorLabels();
    if (!errorLabels) {
        return false;
    }
    for (auto& label : errorLabels.value()) {
        if (label == ErrorLabel::kTransientTransaction) {
            return true;
        }
    }
    return false;
}

void BulkWriteOp::processChildBatchResponseFromRemote(
    const TargetedWriteBatch& writeBatch,
    const AsyncRequestsSender::Response& response,
    boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace) {
    invariant(response.swResponse.getStatus().isOK(), "Response status was unexpectedly not OK");

    auto childBatchResponse = response.swResponse.getValue();
    LOGV2_DEBUG(7279200,
                4,
                "Processing bulk write response from shard.",
                "shard"_attr = response.shardId,
                "response"_attr = childBatchResponse.data);

    auto childBatchStatus = getStatusFromCommandResult(childBatchResponse.data);
    if (childBatchStatus.isOK()) {
        auto bwReply = BulkWriteCommandReply::parse(IDLParserContext("BulkWriteCommandReply"),
                                                    childBatchResponse.data);
        if (bwReply.getWriteConcernError()) {
            saveWriteConcernError(response.shardId, bwReply.getWriteConcernError().value());
        }

        // TODO (SERVER-76958): Iterate through the cursor rather than looking only at the first
        // batch.
        const auto& replyItems = bwReply.getCursor().getFirstBatch();

        // Capture the errors if any exist and mark the writes in the TargetedWriteBatch so that
        // they may be re-targeted if needed.
        noteChildBatchResponse(
            writeBatch, replyItems, bwReply.getRetriedStmtIds(), errorsPerNamespace);
    } else {
        noteChildBatchError(writeBatch, childBatchStatus);

        // If we are in a transaction, we must abort execution on any error.
        // TODO SERVER-72793: handle WouldChangeOwningShard errors.
        if (TransactionRouter::get(_opCtx)) {
            _aborted = true;

            auto errorReply =
                ErrorReply::parse(IDLParserContext("ErrorReply"), childBatchResponse.data);

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

void BulkWriteOp::noteChildBatchResponse(
    const TargetedWriteBatch& targetedBatch,
    const std::vector<BulkWriteReplyItem>& replyItems,
    const boost::optional<std::vector<StmtId>>& retriedStmtIds,
    boost::optional<stdx::unordered_map<NamespaceString, TrackedErrors>&> errorsPerNamespace) {

    int index = -1;
    bool ordered = _clientRequest.getOrdered();
    boost::optional<write_ops::WriteError> lastError;
    for (const auto& write : targetedBatch.getWrites()) {
        ++index;
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];
        // When an error is encountered on an ordered bulk write, it is impossible for any of the
        // remaining operations to have been executed. For that reason we reset them here so they
        // may be retargeted and retried if the error we saw is one we can retry after (e.g.
        // StaleConfig.).
        if (ordered && lastError) {
            invariant(index >= (int)replyItems.size());
            writeOp.resetWriteToReady();
            continue;
        }

        // On most errors (for example, a DuplicateKeyError) unordered bulkWrite on a shard attempts
        // to execute following operations even if a preceding operation errored. This isn't true
        // for StaleConfig or StaleDbVersion errors. On these errors, since the shard knows that
        // following operations will also be stale, it stops right away (except for unordered
        // timeseries inserts, see SERVER-80796).
        // For that reason, although typically we can expect the size of replyItems to match the
        // size of the number of operations sent (even in the case of errors), when a staleness
        // error is received the size of replyItems will be <= the size of the number of operations.
        // When this is the case, we treat all the remaining operations which may not have a
        // replyItem as having failed with a staleness error.
        if (!ordered && lastError &&
            (lastError->getStatus().code() == ErrorCodes::StaleDbVersion ||
             ErrorCodes::isStaleShardVersionError(lastError->getStatus())) &&
            (index == (int)replyItems.size())) {
            // Decrement the index so it keeps pointing to the same error (i.e. the
            // last error, which is a staleness error).
            LOGV2_DEBUG(7695304,
                        4,
                        "Duplicating the error for op",
                        "opIdx"_attr = write->writeOpRef.first,
                        "error"_attr = lastError->getStatus());
            index--;
        }

        auto& reply = replyItems[index];
        if (reply.getStatus().isOK()) {
            writeOp.noteWriteComplete(*write, reply);
        } else {
            lastError.emplace(write->writeOpRef.first, reply.getStatus());
            writeOp.noteWriteError(*write, *lastError);

            auto origWrite = BulkWriteCRUDOp(_clientRequest.getOps()[write->writeOpRef.first]);
            auto nss = _clientRequest.getNsInfo()[origWrite.getNsInfoIdx()].getNs();

            // We don't always want to track errors per-namespace, e.g. when we encounter errors
            // local to mongos.
            if (errorsPerNamespace) {
                if (errorsPerNamespace->find(nss) == errorsPerNamespace->end()) {
                    TrackedErrors trackedErrors;
                    trackedErrors.startTracking(ErrorCodes::StaleConfig);
                    trackedErrors.startTracking(ErrorCodes::StaleDbVersion);
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

    if (retriedStmtIds && !retriedStmtIds->empty()) {
        if (_retriedStmtIds) {
            _retriedStmtIds->insert(
                _retriedStmtIds->end(), retriedStmtIds->begin(), retriedStmtIds->end());
        } else {
            _retriedStmtIds = retriedStmtIds;
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

    noteChildBatchError(batch, status);

    LOGV2_DEBUG(8048100,
                4,
                "Unable to receive bulkWrite results from shard",
                "shardInfo"_attr = shardInfo,
                "error"_attr = redact(status));

    // If we see a local shutdown error, it means mongos itself is shutting down. A remote shutdown
    // error would have been returned with response.swResponse.getStatus() being OK.
    // If we see a local CallbackCanceled error, it is likely also due to mongos shutting down,
    // therefore shutting down executor thread pools and cancelling any work scheduled on them.
    // While we don't currently know of any other cases we'd see CallbackCanceled here, we check
    // the shutdown flag as well to ensure the cancellation is due to shutdown.
    // While the shutdown flag check is deprecated, that is because modules shouldn't consult it
    // to coordinate their own shutdowns. But it is OK to use here because we are only checking
    // whether a shutdown has started.
    if (ErrorCodes::isShutdownError(responseStatus) ||
        (responseStatus == ErrorCodes::CallbackCanceled && globalInShutdownDeprecated())) {
        // We shouldn't continue execution (even if unordered) if we are shutting down since
        // further batches will fail to execute as well.
        _aborted = true;

        // We want to throw such an error at the top level so that it can be returned to the client
        // directly with the appropriate error labels,  allowing them to retry it.
        uassertStatusOK(responseStatus);
    }

    // If we are in a transaction, we must stop immediately (even for unordered).
    if (_inTransaction) {
        // Even if we aren't throwing a top-level error, we won't continue processing any
        // outstanding writes after seeing this error since the transaction is aborted.
        _aborted = true;

        // Throw when there is a transient transaction error as those must be returned to the client
        // at the top level to allow them to retry.
        if (isTransientTransactionError(status.code(), false, false)) {
            uassertStatusOK(status);
        }
    }
}

void BulkWriteOp::noteChildBatchError(const TargetedWriteBatch& targetedBatch,
                                      const Status& status) {
    // Treat an error to get a batch response as failures of the contained write(s).
    const int numErrors = _clientRequest.getOrdered() ? 1 : targetedBatch.getWrites().size();

    std::vector<BulkWriteReplyItem> emulatedReplies;
    emulatedReplies.reserve(numErrors);

    for (int i = 0; i < numErrors; i++) {
        emulatedReplies.emplace_back(i, status);
    }

    // This error isn't actually specific to any namespaces and so we do not want to track it.
    noteChildBatchResponse(targetedBatch,
                           emulatedReplies,
                           /* retriedStmtIds */ boost::none,
                           /* errorsPerNamespace*/ boost::none);
}

void BulkWriteOp::noteWriteOpFinalResponse(
    size_t opIdx,
    const BulkWriteReplyItem& reply,
    const boost::optional<std::vector<StmtId>>& retriedStmtIds) {
    WriteOp& writeOp = _writeOps[opIdx];

    // Cancel all childOps if any.
    writeOp.resetWriteToReady();

    if (reply.getStatus().isOK()) {
        writeOp.setOpComplete(reply);
    } else {
        auto writeError = write_ops::WriteError(opIdx, reply.getStatus());
        writeOp.setOpError(writeError);
    }

    if (retriedStmtIds && !retriedStmtIds->empty()) {
        if (_retriedStmtIds) {
            _retriedStmtIds->insert(
                _retriedStmtIds->end(), retriedStmtIds->begin(), retriedStmtIds->end());
        } else {
            _retriedStmtIds = retriedStmtIds;
        }
    }
}

BulkWriteReplyInfo BulkWriteOp::generateReplyInfo() {
    dassert(isFinished());
    std::vector<BulkWriteReplyItem> replyItems;
    int numErrors = 0;
    replyItems.reserve(_writeOps.size());

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        // If we encountered an error causing us to abort execution we may not have waited for
        // responses to all outstanding requests.
        dassert(writeOp.getWriteState() != WriteOpState_Pending || _aborted);
        if (writeOp.getWriteState() == WriteOpState_Completed) {
            replyItems.push_back(writeOp.takeBulkWriteReplyItem());
        } else if (writeOp.getWriteState() == WriteOpState_Error) {
            replyItems.emplace_back(writeOp.getWriteItem().getItemIndex(),
                                    writeOp.getOpError().getStatus());
            // TODO SERVER-79510: Remove this. This is necessary right now because the nModified
            //  field is lost in the BulkWriteReplyItem -> WriteError transformation but
            // we want to return nModified for failed updates. However, this does not actually
            // return a correct value for multi:true updates that partially succeed (i.e. succeed
            // on one or more shard and fail on one or more shards). In SERVER-79510 we should
            // return a correct nModified count by summing the success responses' nModified
            // values.
            if (writeOp.getWriteItem().getOpType() == BatchedCommandRequest::BatchType_Update) {
                replyItems.back().setNModified(0);
            }
            numErrors++;
            // Only return the first error if we are ordered.
            if (ordered)
                break;
        }
    }

    return {std::move(replyItems), numErrors, generateWriteConcernError(), _retriedStmtIds};
}

void BulkWriteOp::saveWriteConcernError(ShardId shardId, BulkWriteWriteConcernError wcError) {
    WriteConcernErrorDetail wce;
    wce.setStatus(Status(ErrorCodes::Error(wcError.getCode()), wcError.getErrmsg()));
    _wcErrors.push_back(ShardWCError(shardId, wce));
}

void BulkWriteOp::saveWriteConcernError(ShardId shardId, WriteConcernErrorDetail wce) {
    _wcErrors.push_back(ShardWCError(shardId, wce));
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
        // sharded timeseries collections.
        auto errors = errorsPerNamespace.find(nsEntry.getNs());
        if (errors != errorsPerNamespace.cend()) {
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleConfig)) {
                LOGV2_DEBUG(7279201,
                            4,
                            "Noting stale config response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleShardResponse(
                    _opCtx, error.endpoint, *error.error.getStatus().extraInfo<StaleConfigInfo>());
            }
            for (const auto& error : errors->second.getErrors(ErrorCodes::StaleDbVersion)) {
                LOGV2_DEBUG(7279202,
                            4,
                            "Noting stale database response.",
                            "shardId"_attr = error.endpoint.shardName,
                            "status"_attr = error.error.getStatus());
                targeter->noteStaleDbResponse(
                    _opCtx,
                    error.endpoint,
                    *error.error.getStatus().extraInfo<StaleDbRoutingVersion>());
            }
        }
    }
}

int BulkWriteOp::getBaseChildBatchCommandSizeEstimate() const {
    // For simplicity, we build a dummy bulk write command request that contains all the common
    // fields and serialize it to get the base command size.
    // TODO SERVER-78301: Re-evaluate this estimation method and consider switching to a more
    // efficient approach.
    // We only bother to copy over variable-size and/or optional fields, since the value of fields
    // that are fixed-size and always present (e.g. 'ordered') won't affect the size calculation.
    BulkWriteCommandRequest request;

    // These have not been set yet, but will be set later on for each namespace as part of the
    // write targeting and batch building process. To ensure we save space for these fields, we
    // add dummy versions to the namespaces before serializing.
    static const ShardVersion mockShardVersion =
        ShardVersionFactory::make(ChunkVersion::IGNORED(), CollectionIndexes());
    static const DatabaseVersion mockDBVersion = DatabaseVersion(UUID::gen(), Timestamp());

    auto nsInfo = _clientRequest.getNsInfo();
    for (auto& nsEntry : nsInfo) {
        nsEntry.setShardVersion(mockShardVersion);
        nsEntry.setDatabaseVersion(mockDBVersion);
        if (!nsEntry.getNs().isTimeseriesBucketsCollection()) {
            // This could be a timeseries view. To be conservative about the estimate, we
            // speculatively account for the additional size needed for the timeseries bucket
            // transalation and the 'isTimeseriesCollection' field.
            nsEntry.setNs(nsEntry.getNs().makeTimeseriesBucketsNamespace());
            nsEntry.setIsTimeseriesNamespace(true);
        }
    }
    request.setNsInfo(nsInfo);

    request.setDbName(_clientRequest.getDbName());
    request.setDollarTenant(_clientRequest.getDollarTenant());
    request.setLet(_clientRequest.getLet());
    // We'll account for the size to store each individual op as we add them, so just put an empty
    // vector as a placeholder for the array. This will ensure we properly count the size of the
    // field name and the empty array.
    request.setOps({});

    if (_isRetryableWrite) {
        // We'll account for the size to store each individual stmtId as we add ops, so similar to
        // above with ops, we just put an empty vector as a placeholder for now.
        request.setStmtIds({});
    }

    BSONObjBuilder builder;
    request.serialize(BSONObj(), &builder);
    // Add writeConcern and lsid/txnNumber to ensure we save space for them.
    logical_session_id_helpers::serializeLsidAndTxnNumber(_opCtx, &builder);
    builder.append(WriteConcernOptions::kWriteConcernField, _opCtx->getWriteConcern().toBSON());

    return builder.obj().objsize();
}

void addIdsForInserts(BulkWriteCommandRequest& origCmdRequest) {
    std::vector<
        stdx::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>>
        newOps;
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
            auto newOp = BulkWriteInsertOp(insert->getInsert(), std::move(newDoc));
            newOps.push_back(std::move(newOp));
        } else {
            newOps.push_back(std::move(op));
        }
    }

    origCmdRequest.setOps(newOps);
}


}  // namespace bulk_write_exec

}  // namespace mongo
