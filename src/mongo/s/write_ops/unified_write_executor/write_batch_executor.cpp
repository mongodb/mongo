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

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

namespace {
BulkWriteCommandReply parseBulkWriteCommandReply(const BSONObj& responseData) {
    return BulkWriteCommandReply::parse(responseData,
                                        IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));
}
}  // namespace

bool WriteBatchExecutor::usesProvidedRoutingContext(const WriteBatch& batch) const {
    // For SimpleWriteBatches, the executor use the provided RoutingContext. For all other batch
    // types, the executor does not use the provided RoutingContext.
    return std::visit(OverloadedVisitor{[](const SimpleWriteBatch& data) { return true; },
                                        [](const NonTargetedWriteBatch& data) { return false; },
                                        [](const InternalTransactionBatch& data) { return false; },
                                        [](const EmptyBatch& data) {
                                            return false;
                                        }},
                      batch.data);
}

WriteBatchResponse WriteBatchExecutor::execute(OperationContext* opCtx,
                                               RoutingContext& routingCtx,
                                               const WriteBatch& batch) {
    return std::visit(
        [&](const auto& batchData) -> WriteBatchResponse {
            return _execute(opCtx, routingCtx, batchData);
        },
        batch.data);
}

BulkWriteCommandRequest WriteBatchExecutor::buildBulkWriteRequestWithoutTxnInfo(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    std::vector<BulkWriteOpVariant> bulkOps;
    std::vector<NamespaceInfoEntry> nsInfos;
    std::map<NamespaceString, int> nsIndexMap;
    const bool isRetryableWrite = opCtx->isRetryableWrite();
    std::vector<int> stmtIds;
    if (isRetryableWrite) {
        stmtIds.reserve(ops.size());
    }
    for (auto& op : ops) {
        auto bulkOp = op.getBulkWriteOp();
        auto& nss = op.getNss();
        NamespaceInfoEntry nsInfo(nss);
        nsInfo.setCollectionUUID(op.getCollectionUUID());
        nsInfo.setEncryptionInformation(op.getEncryptionInformation());
        if (!versionByNss.empty()) {
            auto versionIt = versionByNss.find(nss);
            tassert(10346801,
                    "The shard version info should be present in the batch",
                    versionIt != versionByNss.end());
            auto& version = versionIt->second;

            nsInfo.setShardVersion(version.shardVersion);
            nsInfo.setDatabaseVersion(version.databaseVersion);
        }

        // Reassigns the namespace index for the list of ops.
        if (nsIndexMap.find(nss) == nsIndexMap.end()) {
            nsIndexMap[nss] = nsInfos.size();
            nsInfos.push_back(nsInfo);
        }
        auto nsIndex = nsIndexMap[nss];
        visit([&](auto& value) { return value.setNsInfoIdx(nsIndex); }, bulkOp);

        if (op.getType() == WriteType::kUpdate &&
            allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
            get_if<BulkWriteUpdateOp>(&bulkOp)->setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
        }

        auto sampleIdIt = sampleIds.find(op.getId());
        if (sampleIdIt != sampleIds.end()) {
            visit(OverloadedVisitor{
                      [&](mongo::BulkWriteInsertOp& op) { return; },
                      [&](mongo::BulkWriteUpdateOp& op) { op.setSampleId(sampleIdIt->second); },
                      [&](mongo::BulkWriteDeleteOp& op) { op.setSampleId(sampleIdIt->second); },
                  },
                  bulkOp);
        }

        bulkOps.emplace_back(bulkOp);

        if (isRetryableWrite) {
            stmtIds.push_back(op.getEffectiveStmtId());
        }
    }

    auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));
    bulkRequest.setOrdered(_cmdRef.getOrdered());
    bulkRequest.setBypassDocumentValidation(_cmdRef.getBypassDocumentValidation());
    bulkRequest.setBypassEmptyTsReplacement(_cmdRef.getBypassEmptyTsReplacement());
    bulkRequest.setLet(_cmdRef.getLet());
    if (_cmdRef.isBulkWriteCommand()) {
        bulkRequest.setErrorsOnly(_cmdRef.getErrorsOnly().value_or(false));
        bulkRequest.setComment(_cmdRef.getComment());
        bulkRequest.setMaxTimeMS(_cmdRef.getMaxTimeMS());
    }

    if (isRetryableWrite) {
        bulkRequest.setStmtIds(stmtIds);
    }
    return bulkRequest;
}

BSONObj WriteBatchExecutor::buildBulkWriteRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::map<WriteOpId, UUID>& sampleIds,
    bool shouldAppendLsidAndTxnNumber,
    bool shouldAppendWriteConcern,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    auto bulkRequest = buildBulkWriteRequestWithoutTxnInfo(
        opCtx, ops, versionByNss, sampleIds, allowShardKeyUpdatesWithoutFullShardKeyInQuery);
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    BSONObjBuilder builder;
    bulkRequest.serialize(&builder);

    if (shouldAppendLsidAndTxnNumber) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    if (shouldAppendWriteConcern) {
        auto writeConcern = getWriteConcernForShardRequest(opCtx);
        // We don't append write concern in a transaction.
        if (writeConcern && !inTransaction) {
            builder.append(WriteConcernOptions::kWriteConcernField, writeConcern->toBSON());
        }
    }

    return builder.obj();
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const EmptyBatch& batch) {
    return EmptyBatchResponse{};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const SimpleWriteBatch& batch) {
    std::vector<AsyncRequestsSender::Request> requestsToSend;
    for (auto& [shardId, shardRequest] : batch.requestByShardId) {
        auto bulkRequestObj = buildBulkWriteRequest(opCtx,
                                                    shardRequest.ops,
                                                    shardRequest.versionByNss,
                                                    shardRequest.sampleIds,
                                                    true /* shouldAppendLsidAndTxnNumber */,
                                                    true /* shouldAppendWriteConcern */);
        LOGV2_DEBUG(10605503,
                    4,
                    "Constructed request for shard",
                    "request"_attr = bulkRequestObj,
                    "shardId"_attr = shardId);
        requestsToSend.emplace_back(shardId, std::move(bulkRequestObj));
    }

    // Note we check this rather than `isRetryableWrite()` because we do not want to retry
    // commands within retryable internal transactions.
    bool shouldRetry = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        std::move(requestsToSend),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        shouldRetry ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

    // For each namespace 'nss' that is used by the current 'batch', inform the RoutingContext
    // that we are sending requests that involve 'nss'.
    for (const auto& nss : batch.getInvolvedNamespaces()) {
        routingCtx.onRequestSentForNss(nss);
    }

    SimpleWriteBatchResponse shardResponses;

    while (!sender.done()) {
        auto arsResponse = sender.next();
        ShardResponse shardResponse{std::move(arsResponse.swResponse),
                                    batch.requestByShardId.at(arsResponse.shardId).ops,
                                    std::move(arsResponse.shardHostAndPort)};
        shardResponses.emplace(std::move(arsResponse.shardId), std::move(shardResponse));
    }

    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            shardResponses.size() == batch.requestByShardId.size());

    return shardResponses;
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const NonTargetedWriteBatch& batch) {
    const WriteOp& writeOp = batch.op;

    bool allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || TransactionRouter::get(opCtx);

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(writeOp.getId(), *batch.sampleId);
    }

    auto cmdObj = buildBulkWriteRequest(opCtx,
                                        {writeOp},
                                        {},        /* versionByNss*/
                                        sampleIds, /* sampleIds */
                                        false,     /* shouldAppendLsidAndTxnNumber */
                                        false,     /* shouldAppendWriteConcern */
                                        allowShardKeyUpdatesWithoutFullShardKeyInQuery);

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
        opCtx, writeOp.getNss(), std::move(cmdObj), wce);

    // If 'swRes' is OK but the response is empty, that means the two-phase write completed
    // successfully without updating or deleting anything (because nothing matched the filter).
    //
    // In this case, we create a BulkWriteCommandReply containing a single reply item with an OK
    // status and with n=0 (and with nModified=0 if 'op' is an update), and then we return a
    // NoRetryWriteBatchResponse containing this BulkWriteCommandReply.
    if (swRes.isOK() && swRes.getValue().getResponse().isEmpty()) {
        BulkWriteReplyItem replyItem(0, swRes.getStatus());
        replyItem.setN(0);
        if (writeOp.getType() == WriteType::kUpdate) {
            replyItem.setNModified(0);
        }

        auto cursor =
            BulkWriteCommandResponseCursor(0 /*cursorId*/,
                                           std::vector<BulkWriteReplyItem>{std::move(replyItem)},
                                           NamespaceString::makeBulkWriteNSS(boost::none));
        return NoRetryWriteBatchResponse{
            BulkWriteCommandReply(std::move(cursor), 0, 0, 0, 0, 0, 0), std::move(wce), writeOp};
    }

    auto swBulkWriteResponse = swRes.isOK()
        ? StatusWith{parseBulkWriteCommandReply(swRes.getValue().getResponse())}
        : StatusWith<BulkWriteCommandReply>{swRes.getStatus()};

    return NoRetryWriteBatchResponse{std::move(swBulkWriteResponse), std::move(wce), writeOp};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const InternalTransactionBatch& batch) {
    const WriteOp& writeOp = batch.op;

    bool allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(writeOp.getId(), *batch.sampleId);
    }

    auto singleUpdateRequest =
        buildBulkWriteRequestWithoutTxnInfo(opCtx,
                                            {writeOp},
                                            {}, /* versionByNss*/
                                            sampleIds,
                                            allowShardKeyUpdatesWithoutFullShardKeyInQuery);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    BulkWriteCommandReply bulkWriteResponse;

    // Execute the singleUpdateRequest (a bulkWrite command) in an internal transaction to
    // perform the retryable timeseries update operation. This separate bulkWrite command will
    // get executed on its own via unified_write_executor::bulkWrite() logic again as a transaction,
    // which handles retries of all kinds. This function is just a client of the internal
    // transaction spawned. As a result, we must only receive a single final (non-retryable)
    // response for the timeseries update operation.
    auto swResult = txn.runNoThrow(
        opCtx, [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto updateResponse = txnClient.runCRUDOpSync(singleUpdateRequest);
            bulkWriteResponse = std::move(updateResponse);

            return SemiFuture<void>::makeReady();
        });

    Status responseStatus = swResult.getStatus();
    WriteConcernErrorDetail wce;
    if (responseStatus.isOK()) {
        wce = swResult.getValue().wcError;
        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
    }

    auto swBulkWriteResponse = responseStatus.isOK()
        ? StatusWith(std::move(bulkWriteResponse))
        : StatusWith<BulkWriteCommandReply>(responseStatus);

    return NoRetryWriteBatchResponse{std::move(swBulkWriteResponse), std::move(wce), writeOp};
}

}  // namespace unified_write_executor
}  // namespace mongo
