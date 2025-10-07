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

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/s/request_types/coordinate_multi_update_gen.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/coordinate_multi_update_util.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

namespace {
void appendWriteConcern(OperationContext* opCtx, BSONObjBuilder& builder) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    auto writeConcern = getWriteConcernForShardRequest(opCtx);
    // We don't append write concern in a transaction.
    if (writeConcern && !inTransaction) {
        builder.append(WriteConcernOptions::kWriteConcernField, writeConcern->toBSON());
    }
}

DatabaseName getExecutionDatabase(const WriteBatch& batch) {
    // If the batch is generated from a findAndModify command, we send to the requested database.
    // Otherwise, we send to admin database because bulkWrite like commands may contain multiple
    // namespaces.
    if (batch.isFindAndModify()) {
        auto namespaces = batch.getInvolvedNamespaces();
        tassert(10394900, "Expected only a single namespace", namespaces.size() == 1);
        return namespaces.begin()->dbName();
    }
    return DatabaseName::kAdmin;
}
}  // namespace

bool WriteBatchExecutor::usesProvidedRoutingContext(const WriteBatch& batch) const {
    // For SimpleWriteBatches, the executor use the provided RoutingContext. For all other batch
    // types, the executor does not use the provided RoutingContext.
    return std::visit(OverloadedVisitor{
                          [](const SimpleWriteBatch& data) { return true; },
                          [](const NonTargetedWriteBatch& data) { return false; },
                          [](const InternalTransactionBatch& data) { return false; },
                          [](const MultiWriteBlockingMigrationsBatch& data) { return false; },
                          [](const EmptyBatch& data) { return false; },
                      },
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
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    FilterGenericArguments filterGenericArguments) const {
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

    if (filterGenericArguments == FilterGenericArguments{true}) {
        coordinate_multi_update_util::filterRequestGenericArguments(
            bulkRequest.getGenericArguments());
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
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    FilterGenericArguments filterGenericArguments,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    BSONObjBuilder builder;
    auto bulkRequest =
        buildBulkWriteRequestWithoutTxnInfo(opCtx,
                                            ops,
                                            versionByNss,
                                            sampleIds,
                                            allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                            filterGenericArguments);
    bulkRequest.serialize(&builder);

    if (shouldAppendLsidAndTxnNumber == ShouldAppendLsidAndTxnNumber{true}) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    if (shouldAppendReadWriteConcern == ShouldAppendReadWriteConcern{true}) {
        appendWriteConcern(opCtx, builder);
    }

    return builder.obj();
}

BSONObj WriteBatchExecutor::buildFindAndModifyRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    tassert(10394901, "Expected a single write op for the findAndModify command", ops.size() == 1);
    const auto& op = ops.front();
    tassert(10394902, "Expected a findAndModify request", op.getCommand().isFindAndModifyCommand());

    // Make a copy of the original request and clear attributes that we may append later.
    auto request = op.getCommand().getFindAndModifyCommandRequest();
    request.setLsid(boost::none);
    request.setTxnNumber(boost::none);
    request.setWriteConcern(boost::none);
    request.setReadConcern(boost::none);

    if (!versionByNss.empty()) {
        auto versionIt = versionByNss.find(op.getNss());
        if (versionIt != versionByNss.end()) {
            request.setShardVersion(versionIt->second.shardVersion);
            request.setDatabaseVersion(versionIt->second.databaseVersion);
        }
    }

    auto sampleIdIt = sampleIds.find(op.getId());
    if (sampleIdIt != sampleIds.end()) {
        request.setSampleId(sampleIdIt->second);
    }

    if (op.getType() == WriteType::kUpdate &&
        allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
        request.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
            *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
    }

    auto cmdObj = CommandHelpers::filterCommandRequestForPassthrough(request.toBSON());

    if (shouldAppendReadWriteConcern == ShouldAppendReadWriteConcern{true}) {
        cmdObj = applyReadWriteConcern(opCtx, true /* appendRC */, true /* appendWC */, cmdObj);
    }

    BSONObjBuilder builder(cmdObj);
    if (shouldAppendLsidAndTxnNumber == ShouldAppendLsidAndTxnNumber{true}) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    return builder.obj();
}

BSONObj WriteBatchExecutor::buildRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    FilterGenericArguments filterGenericArguments,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    tassert(10394903, "Expected at least one write op", !ops.empty());
    if (ops.front().isFindAndModify()) {
        return buildFindAndModifyRequest(opCtx,
                                         ops,
                                         versionByNss,
                                         sampleIds,
                                         allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                         shouldAppendLsidAndTxnNumber,
                                         shouldAppendReadWriteConcern);
    } else {
        return buildBulkWriteRequest(opCtx,
                                     ops,
                                     versionByNss,
                                     sampleIds,
                                     allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                     filterGenericArguments,
                                     shouldAppendLsidAndTxnNumber,
                                     shouldAppendReadWriteConcern);
    }
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
        auto requestObj =
            buildRequest(opCtx,
                         shardRequest.ops,
                         shardRequest.versionByNss,
                         shardRequest.sampleIds,
                         boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                         FilterGenericArguments{false},
                         ShouldAppendLsidAndTxnNumber{true},
                         ShouldAppendReadWriteConcern{true});
        LOGV2_DEBUG(10605503,
                    4,
                    "Constructed request for shard",
                    "request"_attr = requestObj,
                    "shardId"_attr = shardId);
        requestsToSend.emplace_back(shardId, std::move(requestObj));
    }

    // Note we check this rather than `isRetryableWrite()` because we do not want to retry
    // commands within retryable internal transactions.
    bool shouldRetry = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
    auto databaseName = getExecutionDatabase(WriteBatch{batch});
    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        databaseName,
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

    auto cmdObj = buildRequest(opCtx,
                               {writeOp},
                               {},        /* versionByNss*/
                               sampleIds, /* sampleIds */
                               allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                               FilterGenericArguments{false},
                               ShouldAppendLsidAndTxnNumber{false},
                               ShouldAppendReadWriteConcern{false});

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
        opCtx, writeOp.getNss(), std::move(cmdObj), wce);

    auto swResponse = swRes.isOK() ? StatusWith{swRes.getValue().getResponse()}
                                   : StatusWith<BSONObj>{swRes.getStatus()};

    return NoRetryWriteBatchResponse{std::move(swResponse), std::move(wce), writeOp};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const InternalTransactionBatch& batch) {
    const WriteOp& writeOp = batch.op;

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(writeOp.getId(), *batch.sampleId);
    }

    auto cmdObj = buildRequest(opCtx,
                               {writeOp},
                               {},          /* versionByNss*/
                               sampleIds,   /* sampleIds */
                               boost::none, /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */
                               FilterGenericArguments{false},
                               ShouldAppendLsidAndTxnNumber{false},
                               ShouldAppendReadWriteConcern{false});

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);
    BSONObj response;

    // Execute the `cmdObj` in an internal transaction to perform the retryable timeseries update
    // operation. This separate write command will get executed on its own via UWE logic again as a
    // transaction, which handles retries of all kinds. This function is just a client of the
    // internal transaction spawned. As a result, we must only receive a single final
    // (non-retryable) response for the timeseries update operation.
    auto swResult = txn.runNoThrow(
        opCtx, [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto database = getExecutionDatabase(WriteBatch{batch});
            response = txnClient.runCommandSync(database, cmdObj);
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

    auto swResponse =
        responseStatus.isOK() ? StatusWith(response) : StatusWith<BSONObj>(responseStatus);

    return NoRetryWriteBatchResponse{std::move(swResponse), std::move(wce), writeOp};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const MultiWriteBlockingMigrationsBatch& batch) {
    const WriteOp& writeOp = batch.op;
    const auto& nss = writeOp.getNss();

    auto cmdObj = buildRequest(opCtx,
                               {writeOp},
                               {},    /* versionByNss */
                               {},    /* sampleIds */
                               false, /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */
                               FilterGenericArguments{true},
                               ShouldAppendLsidAndTxnNumber{true},
                               ShouldAppendReadWriteConcern{false});

    StatusWith<BSONObj> reply = [&]() {
        try {
            return StatusWith(
                coordinate_multi_update_util::executeCoordinateMultiUpdate(opCtx, nss, cmdObj));
        } catch (const DBException& e) {
            return StatusWith<BSONObj>(e.toStatus());
        }
    }();

    return NoRetryWriteBatchResponse{std::move(reply), boost::none, {writeOp}};
}

}  // namespace unified_write_executor
}  // namespace mongo
