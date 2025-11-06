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

#include "mongo/db/error_labels.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/raw_data_operation.h"
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
BulkWriteCommandReply parseBulkWriteCommandReply(const BSONObj& responseData) {
    return BulkWriteCommandReply::parse(responseData,
                                        IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));
}

BulkWriteCommandReply parseBulkWriteCommandReplySingleOp(const BSONObj& responseData,
                                                         const WriteOp& op) {
    // If we're parsing a BulkWriteCommandReply for a single op and 'responseData' is empty, that
    // means the two-phase write completed successfully without updating or deleting anything
    // (because nothing matched the filter).
    //
    // In this case, we create a BulkWriteCommandReply containing a single response item with an
    // OK status and with n=0 (and with nModified=0 if 'writeOp' is an update).
    if (responseData.isEmpty()) {
        BulkWriteReplyItem replyItem(0, Status::OK());
        replyItem.setN(0);
        if (op.getType() == WriteType::kUpdate) {
            replyItem.setNModified(0);
        }

        auto cursor =
            BulkWriteCommandResponseCursor(/*cursorId*/ 0,
                                           std::vector<BulkWriteReplyItem>{std::move(replyItem)},
                                           NamespaceString::makeBulkWriteNSS(boost::none));
        return BulkWriteCommandReply(std::move(cursor), 0, 0, 0, 0, 0, 0);
    }

    return parseBulkWriteCommandReply(responseData);
}

write_ops::FindAndModifyCommandReply parseFindAndModifyCommandReply(const BSONObj& responseData) {
    // If we're parsing a FindAndModifyCommandReply and 'responseData' is empty, that means the
    // two-phase write completed successfully without updating or deleting anything (because nothing
    // matched the filter).
    //
    // In this case, we create a FindAndModifyCommandReply with an OK status and n=0.
    if (responseData.isEmpty()) {
        write_ops::FindAndModifyLastError lastError(/*n*/ 0);
        lastError.setUpdatedExisting(false);

        write_ops::FindAndModifyCommandReply reply;
        reply.setLastErrorObject(std::move(lastError));
        return reply;
    }

    return write_ops::FindAndModifyCommandReply::parse(
        responseData, IDLParserContext("FindAndModifyCommandReply_UnifiedWriteExec"));
}

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

bool isTransientTxnError(bool inTransaction,
                         const Status& status,
                         boost::optional<const BSONObj&> responseData = boost::none) {
    if (inTransaction && !status.isOK()) {
        if (responseData) {
            return hasTransientTransactionErrorLabel(
                ErrorReply::parse(*responseData, IDLParserContext("ErrorReply_UnifiedWriteExec")));
        } else {
            return isTransientTransactionError(
                status.code(), /*hasWriteConcernError*/ false, /*isCommitOrAbort*/ false);
        }
    }
    return false;
}

template <typename T>
void filterGenericArgumentsForEmbeddedCommand(OperationContext* opCtx, T& request) {
    // When the command is embedded, we should filter out all the generic arguments other than
    // "rawData", which is need to preserve the semantics on the timeseries bucket collection.
    request.setRawData(isRawDataOperation(opCtx));
    coordinate_multi_update_util::filterRequestGenericArguments(request.getGenericArguments());
}
}  // namespace

const Status& BasicResponse::getStatus() const {
    tassert(11272100, "Expected OK status or error", !isEmpty());
    return _swReply->getStatus();
}

CommandReplyVariant& BasicResponse::getReply() {
    tassert(11272101, "Expected OK status", isOK());
    return _swReply->getValue();
}

const CommandReplyVariant& BasicResponse::getReply() const {
    tassert(11272102, "Expected OK status", isOK());
    return _swReply->getValue();
}

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
    bool errorsOnly,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand) const {
    const bool isRetryableWrite = opCtx->isRetryableWrite();

    std::vector<BulkWriteOpVariant> bulkOps;
    std::vector<NamespaceInfoEntry> nsInfos;
    std::map<NamespaceString, int> nsIndexMap;
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
            get<BulkWriteUpdateOp>(bulkOp).setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
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

    bulkRequest.setErrorsOnly(errorsOnly);

    if (_cmdRef.isBulkWriteCommand()) {
        bulkRequest.setComment(_cmdRef.getComment());
        bulkRequest.setMaxTimeMS(_cmdRef.getMaxTimeMS());
    }

    if (isEmbeddedCommand) {
        filterGenericArgumentsForEmbeddedCommand(opCtx, bulkRequest);
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
    bool errorsOnly,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    BSONObjBuilder builder;

    auto bulkRequest =
        buildBulkWriteRequestWithoutTxnInfo(opCtx,
                                            ops,
                                            versionByNss,
                                            sampleIds,
                                            errorsOnly,
                                            allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                            isEmbeddedCommand);
    bulkRequest.serialize(&builder);

    if (shouldAppendLsidAndTxnNumber) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    if (shouldAppendReadWriteConcern) {
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
    IsEmbeddedCommand isEmbeddedCommand,
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

    auto versionIt = versionByNss.find(op.getNss());
    if (versionIt != versionByNss.end()) {
        request.setShardVersion(versionIt->second.shardVersion);
        request.setDatabaseVersion(versionIt->second.databaseVersion);
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

    if (isEmbeddedCommand) {
        filterGenericArgumentsForEmbeddedCommand(opCtx, request);
    }

    auto cmdObj = request.toBSON();

    if (shouldAppendReadWriteConcern) {
        cmdObj = applyReadWriteConcern(opCtx, /*appendRC*/ true, /*appendWC*/ true, cmdObj);
    }

    BSONObjBuilder builder(cmdObj);
    if (shouldAppendLsidAndTxnNumber) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    // Try to preserve the original command name alias to forward to shards.
    cmdObj = builder.obj();
    if (!_originalCmdObj.isEmpty() &&
        _originalCmdObj.firstElementFieldNameStringData() !=
            cmdObj.firstElementFieldNameStringData()) {

        BSONObjBuilder builder;
        BSONObjIterator it(cmdObj);
        BSONElement firstElt = it.next();
        builder.append(_originalCmdObj.firstElementFieldNameStringData(),
                       firstElt.checkAndGetStringData());
        while (it.more()) {
            BSONElement elt = it.next();
            builder.append(elt);
        }
        cmdObj = builder.obj();
    }
    return cmdObj;
}

BSONObj WriteBatchExecutor::buildRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::map<WriteOpId, UUID>& sampleIds,
    bool errorsOnly,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    tassert(10394903, "Expected at least one write op", !ops.empty());
    if (ops.front().isFindAndModify()) {
        return buildFindAndModifyRequest(opCtx,
                                         ops,
                                         versionByNss,
                                         sampleIds,
                                         allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                         isEmbeddedCommand,
                                         shouldAppendLsidAndTxnNumber,
                                         shouldAppendReadWriteConcern);
    } else {
        return buildBulkWriteRequest(opCtx,
                                     ops,
                                     versionByNss,
                                     sampleIds,
                                     errorsOnly,
                                     allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                     isEmbeddedCommand,
                                     shouldAppendLsidAndTxnNumber,
                                     shouldAppendReadWriteConcern);
    }
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const EmptyBatch& batch) {
    return EmptyBatchResponse{};
}

ShardResponse WriteBatchExecutor::makeShardResponse(
    StatusWith<executor::RemoteCommandResponse> swResponse,
    std::vector<WriteOp> ops,
    bool inTransaction,
    bool errorsOnly,
    boost::optional<HostAndPort> hostAndPort,
    boost::optional<const ShardId&> shardId) {
    const bool isFindAndModifyCommand = (ops.size() == 1 && ops.front().isFindAndModify());

    // If there was a local error, return a ShardResponse that reports this local error.
    if (!swResponse.isOK()) {
        // TODO SERVER-105303 Handle interruption/shutdown.
        // TODO SERVER-104122 Support for 'WouldChangeOwningShard' writes.
        const Status& status = swResponse.getStatus();

        LOGV2_DEBUG(10896501,
                    4,
                    "Local error occurred when receiving write results from shard",
                    "error"_attr = redact(status),
                    "shardId"_attr = shardId,
                    "host"_attr = hostAndPort);

        const bool transientTxnError = isTransientTxnError(inTransaction, status);

        return ShardResponse{StatusWith<CommandReplyVariant>{status},
                             boost::none /*wce*/,
                             std::move(ops),
                             transientTxnError,
                             errorsOnly,
                             std::move(hostAndPort)};
    }

    const auto& response = swResponse.getValue();
    auto status = getStatusFromCommandResult(response.data);
    boost::optional<WriteConcernErrorDetail> wce;

    // If this is a findAndModify command, populate 'wce' now.
    if (isFindAndModifyCommand) {
        if (auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);
            !wcStatus.isOK()) {
            wce = WriteConcernErrorDetail{std::move(wcStatus)};
        }
    }

    // If there was a top-level error, return a ShardResponse that reports this top-level error.
    if (!status.isOK()) {
        if (!isFindAndModifyCommand) {
            status = status.withContext(str::stream() << "cluster write results unavailable from "
                                                      << response.target);
        }

        LOGV2_DEBUG(10347001,
                    4,
                    "Remote error reported from shard when receiving cluster write results",
                    "error"_attr = redact(status),
                    "shardId"_attr = shardId,
                    "host"_attr = response.target);

        const bool transientTxnError = isTransientTxnError(inTransaction, status, response.data);

        return ShardResponse{StatusWith<CommandReplyVariant>{std::move(status)},
                             std::move(wce),
                             std::move(ops),
                             transientTxnError,
                             errorsOnly,
                             response.target};
    }

    // If there were no local errors or top-level errors, parse the reply, and then return a
    // ShardResponse that contains the parsed reply.
    LOGV2_DEBUG(10347003,
                4,
                "Parsing cluster write shard response",
                "response"_attr = response.data,
                "host"_attr = response.target);

    auto swReply = [&]() -> StatusWith<CommandReplyVariant> {
        if (isFindAndModifyCommand) {
            return parseFindAndModifyCommandReply(
                CommandHelpers::filterCommandReplyForPassthrough(response.data));
        } else {
            auto parsedReply = parseBulkWriteCommandReply(response.data);

            // If this is not a findAndModify command, then we populate 'wce' here.
            if (auto wcError = parsedReply.getWriteConcernError()) {
                wce = WriteConcernErrorDetail{
                    Status(ErrorCodes::Error(wcError->getCode()), wcError->getErrmsg())};
            }

            return std::move(parsedReply);
        }
    }();

    return ShardResponse{std::move(swReply),
                         std::move(wce),
                         std::move(ops),
                         false /*transientTxnError*/,
                         errorsOnly,
                         response.target};
}

ShardResponse WriteBatchExecutor::makeEmptyShardResponse(std::vector<WriteOp> ops,
                                                         bool errorsOnly) {
    return ShardResponse{boost::none /*swReply*/,
                         boost::none /*wce*/,
                         std::move(ops),
                         false /*transientTxnError*/,
                         errorsOnly};
}

NoRetryWriteBatchResponse WriteBatchExecutor::makeNoRetryWriteBatchResponse(
    const StatusWith<BSONObj>& swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction,
    bool errorsOnly) {
    const auto& status = swResponse.getStatus();
    const bool transientTxnError = isTransientTxnError(inTransaction, status);

    auto swReply = [&]() -> StatusWith<CommandReplyVariant> {
        if (status.isOK()) {
            const auto& response = swResponse.getValue();
            return op.isFindAndModify()
                ? CommandReplyVariant{parseFindAndModifyCommandReply(response)}
                : CommandReplyVariant{parseBulkWriteCommandReplySingleOp(response, op)};
        } else {
            return status;
        }
    }();

    return NoRetryWriteBatchResponse{std::move(swReply),
                                     std::move(wce),
                                     std::vector<WriteOp>{op},
                                     transientTxnError,
                                     errorsOnly};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const SimpleWriteBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    std::vector<AsyncRequestsSender::Request> requestsToSend;
    absl::flat_hash_map<ShardId, bool> errorsOnlyByShardId;

    for (auto& [shardId, shardRequest] : batch.requestByShardId) {
        // Determine if the "errorsOnly" parameter should be set to true or false when sending a
        // command to 'shardId'.
        const bool errorsOnly = getErrorsOnlyForShardRequest(shardRequest.ops);
        errorsOnlyByShardId[shardId] = errorsOnly;

        auto requestObj =
            buildRequest(opCtx,
                         shardRequest.ops,
                         shardRequest.versionByNss,
                         shardRequest.sampleIds,
                         errorsOnly,
                         boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                         IsEmbeddedCommand::No,
                         ShouldAppendLsidAndTxnNumber::Yes,
                         ShouldAppendReadWriteConcern::Yes);

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
        shouldRetry ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kStrictlyNotIdempotent);

    // For each namespace 'nss' that is used by the current 'batch', inform the RoutingContext
    // that we are sending requests that involve 'nss'.
    for (const auto& nss : batch.getInvolvedNamespaces()) {
        routingCtx.onRequestSentForNss(nss);
    }

    SimpleWriteBatchResponse shardResponses;
    bool stopParsingResponses = false;

    while (!stopParsingResponses && !sender.done()) {
        auto arsResponse = sender.next();
        auto& shardId = arsResponse.shardId;

        const auto& shardRequest = batch.requestByShardId.at(shardId);
        const bool errorsOnly = errorsOnlyByShardId.at(shardId);

        ShardResponse shardResponse = makeShardResponse(std::move(arsResponse.swResponse),
                                                        shardRequest.ops,
                                                        inTransaction,
                                                        errorsOnly,
                                                        std::move(arsResponse.shardHostAndPort),
                                                        shardId);

        const bool hasTransientTxnError = shardResponse.hasTransientTxnError();
        const bool hasTopLevelErrorThatAbortsTxn =
            (inTransaction && shardResponse.isError() && !hasTransientTxnError &&
             !shardResponse.isWouldChangeOwningShardError());

        const auto numItemErrors = shardResponse.isOK() &&
                holds_alternative<BulkWriteCommandReply>(shardResponse.getReply())
            ? get<BulkWriteCommandReply>(shardResponse.getReply()).getNErrors()
            : 0;

        const bool hasItemErrorThatAbortsTxn = inTransaction && numItemErrors > 0;

        if (hasTransientTxnError || hasTopLevelErrorThatAbortsTxn || hasItemErrorThatAbortsTxn) {
            LOGV2_DEBUG(11272105,
                        4,
                        "Stopped parsing of shard responses due to error in transaction",
                        "topLevelStatus"_attr = shardResponse.getStatus(),
                        "numItemErrors"_attr = numItemErrors,
                        "shardId"_attr = shardId,
                        "host"_attr = shardResponse.getHostAndPort());

            stopParsingResponses = true;
        }

        shardResponses.emplace_back(std::move(shardId), std::move(shardResponse));
    }

    // If we stopped parsing responses early, generate empty ShardResponses for the remaining
    // ShardIds.
    if (stopParsingResponses) {
        // Make a set of the ShardIds for which we already have ShardResponses.
        absl::flat_hash_set<ShardId> shardIdSet;
        for (const auto& [shardId, _] : shardResponses) {
            shardIdSet.emplace(shardId);
        }

        // For each 'shardId' that isn't in 'shardIdSet', create an empty ShardResponse and
        // add it to 'shardResponses'.
        for (const auto& [shardId, _] : batch.requestByShardId) {
            if (!shardIdSet.count(shardId)) {
                const auto& shardRequest = batch.requestByShardId.at(shardId);
                const bool errorsOnly = errorsOnlyByShardId.at(shardId);
                shardResponses.emplace_back(shardId,
                                            makeEmptyShardResponse(shardRequest.ops, errorsOnly));
            }
        }
    }

    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            shardResponses.size() == batch.requestByShardId.size());

    return shardResponses;
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const NonTargetedWriteBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    const WriteOp& writeOp = batch.op;

    bool allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || TransactionRouter::get(opCtx);

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(writeOp.getId(), *batch.sampleId);
    }

    // Determine if the "errorsOnly" parameter should be set to true or false when sending a command
    // to the shards.
    const bool errorsOnly = getErrorsOnlyForShardRequest({writeOp});

    auto cmdObj = buildRequest(opCtx,
                               {writeOp},
                               {}, /* versionByNss */
                               sampleIds,
                               errorsOnly,
                               allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                               IsEmbeddedCommand::Yes,
                               ShouldAppendLsidAndTxnNumber::No,
                               ShouldAppendReadWriteConcern::No);

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes = write_without_shard_key::runTwoPhaseWriteProtocol(
        opCtx, writeOp.getNss(), std::move(cmdObj), wce);

    auto swResponse = swRes.isOK() ? StatusWith{swRes.getValue().getResponse().getOwned()}
                                   : StatusWith<BSONObj>{swRes.getStatus()};

    return makeNoRetryWriteBatchResponse(
        swResponse, std::move(wce), writeOp, inTransaction, errorsOnly);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const InternalTransactionBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    const WriteOp& writeOp = batch.op;

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(writeOp.getId(), *batch.sampleId);
    }

    // Determine if the "errorsOnly" parameter should be set to true or false when sending a command
    // to the shards.
    const bool errorsOnly = getErrorsOnlyForShardRequest({writeOp});

    tassert(11288300, "Unexpected findAndModify command type", !batch.isFindAndModify());
    auto singleUpdateRequest = buildBulkWriteRequestWithoutTxnInfo(
        opCtx,
        {writeOp},
        {}, /* versionByNss */
        sampleIds,
        errorsOnly,
        boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
        IsEmbeddedCommand::No);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, /*resourceYielder*/ nullptr, inlineExecutor);
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
    boost::optional<WriteConcernErrorDetail> wce;
    if (responseStatus.isOK()) {
        if (!swResult.getValue().wcError.toStatus().isOK()) {
            wce = swResult.getValue().wcError;
        }

        if (!swResult.getValue().cmdStatus.isOK()) {
            responseStatus = swResult.getValue().cmdStatus;
        }
    }

    auto swResponse = responseStatus.isOK() ? StatusWith(bulkWriteResponse.toBSON().getOwned())
                                            : StatusWith<BSONObj>(responseStatus);

    return makeNoRetryWriteBatchResponse(
        swResponse, std::move(wce), writeOp, inTransaction, errorsOnly);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const MultiWriteBlockingMigrationsBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    const WriteOp& writeOp = batch.op;
    const auto& nss = writeOp.getNss();

    // Determine if the "errorsOnly" parameter should be set to true or false when sending a command
    // to the shards.
    const bool errorsOnly = getErrorsOnlyForShardRequest({writeOp});

    auto cmdObj = buildRequest(opCtx,
                               {writeOp},
                               {}, /* versionByNss */
                               {}, /* sampleIds */
                               errorsOnly,
                               boost::none, /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */
                               IsEmbeddedCommand::Yes,
                               ShouldAppendLsidAndTxnNumber::Yes,
                               ShouldAppendReadWriteConcern::No);

    StatusWith<BSONObj> reply = [&]() {
        try {
            return StatusWith(
                coordinate_multi_update_util::executeCoordinateMultiUpdate(opCtx, nss, cmdObj));
        } catch (const DBException& e) {
            return StatusWith<BSONObj>(e.toStatus());
        }
    }();

    return makeNoRetryWriteBatchResponse(
        reply, /*wce*/ boost::none, writeOp, inTransaction, errorsOnly);
}

bool WriteBatchExecutor::getErrorsOnlyForShardRequest(const std::vector<WriteOp>& ops) const {
    return _cmdRef.visitRequest(OverloadedVisitor(
        [&](const BatchedCommandRequest&) {
            // For BatchedCommandRequests, we set "errorsOnly" to false if the command has upsert
            // ops (because we need info from the individual reply items to construct the response
            // for the client), otherwise we set "errorsOnly" to true.
            return std::none_of(ops.begin(), ops.end(), [](auto&& op) { return op.isUpsert(); });
        },
        [&](const BulkWriteCommandRequest&) { return _cmdRef.getErrorsOnly().value_or(false); },
        [&](const write_ops::FindAndModifyCommandRequest&) { return false; }));
}

}  // namespace unified_write_executor
}  // namespace mongo
