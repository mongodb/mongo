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
#include <boost/preprocessor/control/iif.hpp>
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
#include "mongo/s/async_requests_sender.h"
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
#include "mongo/util/assert_util.h"
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

// Send and process the child batches. Each child batch is targeted at a unique shard: therefore
// one shard will have only one batch incoming.
void executeChildBatches(OperationContext* opCtx,
                         const std::vector<std::unique_ptr<NSTargeter>>& targeters,
                         TargetedBatchMap& childBatches,
                         BulkWriteOp& bulkWriteOp,
                         stdx::unordered_map<NamespaceString, TrackedErrors>& errorsPerNamespace) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (auto& childBatch : childBatches) {
        auto request = [&]() {
            auto bulkReq = bulkWriteOp.buildBulkCommandRequest(targeters, *childBatch.second);

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
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(childBatch.second->getEstimatedSizeBytes() >= obj.objsize());
            return obj;
        }();

        requests.emplace_back(childBatch.first, request);
    }

    // Use MultiStatementTransactionRequestsSender to send any ready sub-batches to targeted
    // shard endpoints. Requests are sent on construction.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        requests,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        opCtx->isRetryableWrite() ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

    while (!ars.done()) {
        // Block until a response is available.
        auto response = ars.next();

        Status responseStatus = response.swResponse.getStatus();
        // When the responseStatus is not OK, this means that mongos was unable to receive a
        // response from the shard the write batch was sent to, or mongos faced some other local
        // error (for example, mongos was shutting down). In these cases, throw and return the
        // error to the client.
        // Note that this is different from an operation within the bulkWrite command having an
        // error.
        uassertStatusOK(responseStatus);

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

void fillOKInsertReplies(BulkWriteReplyInfo& replies, int size) {
    replies.first.reserve(size);
    for (int i = 0; i < size; ++i) {
        BulkWriteReplyItem reply;
        reply.setN(1);
        reply.setOk(1);
        reply.setIdx(i);
        replies.first.push_back(reply);
    }
}

BulkWriteReplyInfo processFLEResponse(const BulkWriteCRUDOp::OpType& firstOpType,
                                      const BatchedCommandResponse& response) {
    BulkWriteReplyInfo replies;
    if (response.toStatus().isOK()) {
        if (firstOpType == BulkWriteCRUDOp::kInsert) {
            fillOKInsertReplies(replies, response.getN());
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
            replies.first.push_back(reply);
        }
    } else {
        if (response.isErrDetailsSet()) {
            const auto& errDetails = response.getErrDetails();
            if (firstOpType == BulkWriteCRUDOp::kInsert) {
                fillOKInsertReplies(replies, response.getN() + errDetails.size());
                for (const auto& err : errDetails) {
                    int32_t idx = err.getIndex();
                    replies.first[idx].setN(0);
                    replies.first[idx].setOk(0);
                    replies.first[idx].setStatus(err.getStatus());
                }
            } else {
                invariant(errDetails.size() == 1 && response.getN() == 0);
                BulkWriteReplyItem reply(0, errDetails[0].getStatus());
                reply.setN(0);
                if (firstOpType == BulkWriteCRUDOp::kUpdate) {
                    reply.setNModified(0);
                }
                replies.first.push_back(reply);
            }
            replies.second += errDetails.size();
        } else if (response.isWriteConcernErrorSet()) {
            // TODO SERVER-76954 handle write concern errors, use getWriteConcernError.
        } else {
            // response.toStatus() is not OK but there is no errDetails or writeConcernError, so the
            // top level status should be not OK instead. Raising an exception.
            uassertStatusOK(response.getTopLevelStatus());
            MONGO_UNREACHABLE;
        }
    }
    return replies;
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
            return {FLEBatchResult::kNotProcessed, {}};
        }

        BulkWriteReplyInfo replies = processFLEResponse(firstOpType, response);
        return {FLEBatchResult::kProcessed, std::move(replies)};
    } catch (const DBException& ex) {
        LOGV2_WARNING(7749700,
                      "Failed to process bulkWrite with Queryable Encryption",
                      "error"_attr = redact(ex));
        // If Queryable encryption adds support for update with multi: true, we might have to update
        // the way we make replies here to handle SERVER-15292 correctly.
        BulkWriteReplyInfo replies;
        BulkWriteReplyItem reply(0, ex.toStatus());
        reply.setN(0);
        if (firstOpType == BulkWriteCRUDOp::kUpdate) {
            reply.setNModified(0);
        }

        replies.first.push_back(reply);
        replies.second = 1;
        return {FLEBatchResult::kProcessed, std::move(replies)};
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
    Status responseStatus = Status::OK();
    if (!swResult.isOK()) {
        responseStatus = swResult.getStatus();
    } else {
        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
        if (auto wcError = swResult.getValue().wcError; !wcError.toStatus().isOK()) {
            // TODO (SERVER-76954): Handle write concern errors.
        }
    }
    // TODO (SERVER-80481): Handle local error correctly.
    uassertStatusOK(responseStatus);

    auto cursor = bulkWriteResponse.getCursor();
    const auto& replyItems = cursor.getFirstBatch();

    // We should get back just one reply item for the single update we are running.
    tassert(7934203, "unexpected reply for retryable timeseries update", replyItems.size() == 1);

    LOGV2_DEBUG(7934204,
                4,
                "Processing bulk write response for retryable timeseries update",
                "opIdx"_attr = opIdx,
                "singleUpdateRequest"_attr = redact(singleUpdateRequest.toBSON({})),
                "replyItem"_attr = replyItems[0]);
    bulkWriteOp.noteWriteOpFinalResponse(opIdx, replyItems[0]);
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
            } else {
                // Send the child batches and wait for responses.
                executeChildBatches(
                    opCtx, targeters, childBatches, bulkWriteOp, errorsPerNamespace);
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

        LOGV2_DEBUG(7934202,
                    2,
                    "Completed a round of bulkWrite execution",
                    "rounds"_attr = rounds,
                    "numCompletedOps"_attr = numCompletedOps,
                    "targeterChanged"_attr = targeterChanged,
                    "numRoundsWithoutProgress"_attr = numRoundsWithoutProgress);

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
        getBaseBatchCommandSizeEstimate(),
        targetedBatches);
}

BulkWriteCommandRequest BulkWriteOp::buildBulkCommandRequest(
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    const TargetedWriteBatch& targetedBatch) const {
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
        // When an error is encountered on an ordered bulk write, it is impossible for any of the
        // remaining operations to have been executed. For that reason we cancel them here so they
        // may be retargeted and retried.
        if (ordered && lastError) {
            invariant(index >= (int)replyItems.size());
            writeOp.cancelWrites();
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

void BulkWriteOp::noteWriteOpFinalResponse(size_t opIdx, const BulkWriteReplyItem& reply) {
    WriteOp& writeOp = _writeOps[opIdx];

    // Cancel all childOps if any.
    writeOp.cancelWrites();

    if (reply.getStatus().isOK()) {
        writeOp.setOpComplete(reply);
    } else {
        auto writeError = write_ops::WriteError(opIdx, reply.getStatus());
        writeOp.setOpError(writeError);
    }
}

BulkWriteReplyInfo BulkWriteOp::generateReplyInfo() {
    dassert(isFinished());
    std::vector<BulkWriteReplyItem> replyItems;
    int numErrors = 0;
    replyItems.reserve(_writeOps.size());

    const auto ordered = _clientRequest.getOrdered();
    for (auto& writeOp : _writeOps) {
        dassert(writeOp.getWriteState() != WriteOpState_Pending);
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

    return {replyItems, numErrors};
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

int BulkWriteOp::getBaseBatchCommandSizeEstimate() const {
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
    for (auto& ns : nsInfo) {
        ns.setShardVersion(mockShardVersion);
        ns.setDatabaseVersion(mockDBVersion);
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
