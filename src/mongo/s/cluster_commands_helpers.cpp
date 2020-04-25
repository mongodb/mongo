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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/s/cluster_commands_helpers.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/create_database_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

WriteConcernErrorDetail getWriteConcernErrorDetail(const BSONElement& wcErrorElem) {
    WriteConcernErrorDetail wcError;
    std::string errMsg;
    auto wcErrorObj = wcErrorElem.Obj();
    if (!wcError.parseBSON(wcErrorObj, &errMsg)) {
        wcError.clear();
        wcError.setStatus({ErrorCodes::FailedToParse,
                           "Failed to parse writeConcernError: " + wcErrorObj.toString() +
                               ", Received error: " + errMsg});
    }

    return wcError;
}

void appendWriteConcernErrorToCmdResponse(const ShardId& shardId,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder) {
    WriteConcernErrorDetail wcError = getWriteConcernErrorDetail(wcErrorElem);

    auto status = wcError.toStatus();
    wcError.setStatus(
        status.withReason(str::stream() << status.reason() << " at " << shardId.toString()));

    responseBuilder.append("writeConcernError", wcError.toBSON());
}

std::unique_ptr<WriteConcernErrorDetail> getWriteConcernErrorDetailFromBSONObj(const BSONObj& obj) {
    BSONElement wcErrorElem;
    Status status = bsonExtractTypedField(obj, "writeConcernError", Object, &wcErrorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return nullptr;
        } else {
            uassertStatusOK(status);
        }
    }

    return std::make_unique<WriteConcernErrorDetail>(getWriteConcernErrorDetail(wcErrorElem));
}

namespace {

const auto kAllowImplicitCollectionCreation = "allowImplicitCollectionCreation"_sd;

/**
 * Constructs a requests vector targeting each of the specified shard ids. Each request contains the
 * same cmdObj combined with the default sharding parameters.
 */
std::vector<AsyncRequestsSender::Request> buildUnversionedRequestsForShards(
    OperationContext* opCtx, std::vector<ShardId> shardIds, const BSONObj& cmdObj) {
    auto cmdToSend = cmdObj;

    std::vector<AsyncRequestsSender::Request> requests;
    for (auto&& shardId : shardIds)
        requests.emplace_back(std::move(shardId), cmdToSend);

    return requests;
}

std::vector<AsyncRequestsSender::Request> buildUnversionedRequestsForAllShards(
    OperationContext* opCtx, const BSONObj& cmdObj) {
    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload(&shardIds);
    return buildUnversionedRequestsForShards(opCtx, std::move(shardIds), cmdObj);
}

std::vector<AsyncRequestsSender::Response> gatherResponsesImpl(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    bool throwOnStaleShardVersionErrors) {

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        dbName,
        requests,
        readPref,
        retryPolicy);

    // Get the responses.

    std::vector<AsyncRequestsSender::Response> responses;  // Stores results by ShardId

    while (!ars.done()) {
        auto response = ars.next();

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            // Check for special errors that require throwing out any accumulated results.
            auto& responseObj = response.swResponse.getValue().data;
            status = getStatusFromCommandResult(responseObj);

            // If we specify to throw on stale shard version errors, then we will early exit
            // from examining results. Otherwise, we will allow stale shard version errors to
            // accumulate in the list of results.
            if (throwOnStaleShardVersionErrors &&
                ErrorCodes::isStaleShardVersionError(status.code())) {
                uassertStatusOK(status.withContext(str::stream()
                                                   << "got stale shardVersion response from shard "
                                                   << response.shardId << " at host "
                                                   << response.shardHostAndPort->toString()));
            }
            if (ErrorCodes::StaleDbVersion == status) {
                uassertStatusOK(status.withContext(
                    str::stream() << "got stale databaseVersion response from shard "
                                  << response.shardId << " at host "
                                  << response.shardHostAndPort->toString()));
            }

            // In the case a read is performed against a view, the server can return an error
            // indicating that the underlying collection may be sharded. When this occurs the return
            // message will include an expanded view definition and collection namespace. We pass
            // the definition back to the caller by throwing the error. This allows the caller to
            // rewrite the request as an aggregation and retry it.
            if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == status) {
                uassertStatusOK(status);
            }
        }
        responses.push_back(std::move(response));
    }

    return responses;
}
}  // namespace

std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests) {
    return gatherResponsesImpl(
        opCtx, dbName, readPref, retryPolicy, requests, true /* throwOnStaleShardVersionErrors */);
}

std::vector<AsyncRequestsSender::Response> gatherResponsesNoThrowOnStaleShardVersionErrors(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests) {
    return gatherResponsesImpl(
        opCtx, dbName, readPref, retryPolicy, requests, false /* throwOnStaleShardVersionErrors */);
}

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo) {
    return appendDbVersionIfPresent(std::move(cmdObj), dbInfo.databaseVersion());
}

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, DatabaseVersion dbVersion) {
    if (databaseVersion::isFixed(dbVersion)) {
        return cmdObj;
    }
    BSONObjBuilder cmdWithDbVersion(std::move(cmdObj));
    cmdWithDbVersion.append("databaseVersion", dbVersion.toBSON());
    return cmdWithDbVersion.obj();
}

BSONObj appendShardVersion(BSONObj cmdObj, ChunkVersion version) {
    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    version.appendToCommand(&cmdWithVersionBob);
    return cmdWithVersionBob.obj();
}

BSONObj appendAllowImplicitCreate(BSONObj cmdObj, bool allow) {
    BSONObjBuilder newCmdBuilder(std::move(cmdObj));
    newCmdBuilder.append(kAllowImplicitCollectionCreation, allow);
    return newCmdBuilder.obj();
}

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              bool appendRC,
                              bool appendWC,
                              const BSONObj& cmdObj) {
    if (TransactionRouter::get(opCtx)) {
        // When running in a transaction, the rules are:
        // - Never apply writeConcern.  Applying writeConcern to terminal operations such as
        //   abortTransaction and commitTransaction is done directly by the TransactionRouter.
        // - Apply readConcern only if this is the first operation in the transaction.

        if (!opCtx->isStartingMultiDocumentTransaction()) {
            // Cannot apply either read or writeConcern, so short-circuit.
            return cmdObj;
        }

        if (!appendRC) {
            // First operation in transaction, but the caller has not requested readConcern be
            // applied, so there's nothing to do.
            return cmdObj;
        }

        // First operation in transaction, so ensure that writeConcern is not applied, then continue
        // and apply the readConcern.
        appendWC = false;
    }

    // Append all original fields except the readConcern/writeConcern field to the new command.
    BSONObjBuilder output;
    bool seenReadConcern = false;
    bool seenWriteConcern = false;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (appendRC && name == repl::ReadConcernArgs::kReadConcernFieldName) {
            seenReadConcern = true;
        }
        if (appendWC && name == WriteConcernOptions::kWriteConcernField) {
            seenWriteConcern = true;
        }
        if (!output.hasField(name)) {
            output.append(elem);
        }
    }

    // Finally, add the new read/write concern.
    if (appendRC && !seenReadConcern) {
        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        output.appendElements(readConcernArgs.toBSON());
    }
    if (appendWC && !seenWriteConcern) {
        output.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());
    }

    return output.obj();
}

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              CommandInvocation* invocation,
                              const BSONObj& cmdObj) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    const auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
    return applyReadWriteConcern(opCtx,
                                 readConcernSupport.readConcernSupport.isOK(),
                                 invocation->supportsWriteConcern(),
                                 cmdObj);
}

BSONObj applyReadWriteConcern(OperationContext* opCtx, BasicCommand* cmd, const BSONObj& cmdObj) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    const auto readConcernSupport = cmd->supportsReadConcern(cmdObj, readConcernArgs.getLevel());
    return applyReadWriteConcern(opCtx,
                                 readConcernSupport.readConcernSupport.isOK(),
                                 cmd->supportsWriteConcern(cmdObj),
                                 cmdObj);
}

BSONObj stripWriteConcern(const BSONObj& cmdObj) {
    BSONObjBuilder output;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == WriteConcernOptions::kWriteConcernField) {
            continue;
        }
        output.append(elem);
    }
    return output.obj();
}

std::vector<AsyncRequestsSender::Request> buildVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation) {

    auto cmdToSend = cmdObj;

    if (!routingInfo.cm()) {
        // The collection is unsharded. Target only the primary shard for the database.

        const auto primaryShardId = routingInfo.db().primaryId();

        if (shardsToSkip.find(primaryShardId) != shardsToSkip.end()) {
            return {};
        }

        // Attach shardVersion "UNSHARDED", unless targeting the config server.
        const auto cmdObjWithShardVersion = (primaryShardId != "config")
            ? appendShardVersion(cmdToSend, ChunkVersion::UNSHARDED())
            : cmdToSend;

        return buildUnversionedRequestsForShards(
            opCtx,
            {primaryShardId},
            appendDbVersionIfPresent(cmdObjWithShardVersion, routingInfo.db()));
    }

    std::vector<AsyncRequestsSender::Request> requests;

    // The collection is sharded. Target all shards that own chunks that match the query.
    std::set<ShardId> shardIds;
    routingInfo.cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);

    for (const ShardId& shardId : shardIds) {
        if (shardsToSkip.find(shardId) == shardsToSkip.end()) {
            requests.emplace_back(
                shardId, appendShardVersion(cmdToSend, routingInfo.cm()->getVersion(shardId)));
        }
    }

    return requests;
}

std::vector<AsyncRequestsSender::Response> scatterGatherUnversionedTargetAllShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    return gatherResponses(
        opCtx, dbName, readPref, retryPolicy, buildUnversionedRequestsForAllShards(opCtx, cmdObj));
}

std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation) {
    const auto requests = buildVersionedRequestsForTargetedShards(
        opCtx, nss, routingInfo, {} /* shardsToSkip */, cmdObj, query, collation);

    return gatherResponses(opCtx, dbName, readPref, retryPolicy, requests);
}

std::vector<AsyncRequestsSender::Response>
scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
    OperationContext* opCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation) {
    const auto requests = buildVersionedRequestsForTargetedShards(
        opCtx, nss, routingInfo, shardsToSkip, cmdObj, query, collation);

    return gatherResponsesNoThrowOnStaleShardVersionErrors(
        opCtx, dbName, readPref, retryPolicy, requests);
}

std::vector<AsyncRequestsSender::Response> scatterGatherOnlyVersionIfUnsharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::set<ErrorCodes::Error>& ignorableErrors) {
    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // An unversioned request on a sharded collection can cause a shard that has not owned data
        // for the collection yet to implicitly create the collection without all the collection
        // options. So, we signal to shards that they should not implicitly create the collection.
        requests = buildUnversionedRequestsForAllShards(opCtx, cmdObj);
    } else {
        requests = buildVersionedRequestsForTargetedShards(
            opCtx, nss, routingInfo, {} /* shardsToSkip */, cmdObj, BSONObj(), BSONObj());
    }

    return gatherResponses(opCtx, nss.db(), readPref, retryPolicy, requests);
}

AsyncRequestsSender::Response executeCommandAgainstDatabasePrimary(
    OperationContext* opCtx,
    StringData dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    auto responses =
        gatherResponses(opCtx,
                        dbName,
                        readPref,
                        retryPolicy,
                        buildUnversionedRequestsForShards(
                            opCtx, {dbInfo.primaryId()}, appendDbVersionIfPresent(cmdObj, dbInfo)));
    return std::move(responses.front());
}

AsyncRequestsSender::Response executeCommandAgainstShardWithMinKeyChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {

    const auto query = routingInfo.cm()
        ? routingInfo.cm()->getShardKeyPattern().getKeyPattern().globalMin()
        : BSONObj();

    auto responses =
        gatherResponses(opCtx,
                        nss.db(),
                        readPref,
                        retryPolicy,
                        buildVersionedRequestsForTargetedShards(opCtx,
                                                                nss,
                                                                routingInfo,
                                                                {} /* shardsToSkip */,
                                                                cmdObj,
                                                                query,
                                                                BSONObj() /* collation */));
    return std::move(responses.front());
}

RawResponsesResult appendRawResponses(
    OperationContext* opCtx,
    std::string* errmsg,
    BSONObjBuilder* output,
    const std::vector<AsyncRequestsSender::Response>& shardResponses) {

    std::vector<AsyncRequestsSender::Response> successARSResponses;
    std::vector<std::pair<ShardId, BSONObj>> successResponsesReceived;
    std::vector<std::pair<ShardId, Status>> shardNotFoundErrorsReceived;

    // "Generic errors" are all errors that are not shardNotFound errors.
    std::vector<std::pair<ShardId, Status>> genericErrorsReceived;
    std::set<ShardId> shardsWithSuccessResponses;

    boost::optional<Status> firstStaleConfigErrorReceived;
    boost::optional<std::pair<ShardId, BSONElement>> firstWriteConcernErrorReceived;

    const auto processError = [&](const ShardId& shardId, const Status& status) {
        invariant(!status.isOK());
        // It is safe to pass `hasWriteConcernError` as false in the below check because operations
        // run inside transactions do not wait for write concern, except for commit and abort.
        if (TransactionRouter::get(opCtx) &&
            isTransientTransactionError(
                status.code(), false /*hasWriteConcernError*/, false /*isCommitOrAbort*/)) {
            // Re-throw on transient transaction errors to make sure appropriate error labels are
            // appended to the result.
            uassertStatusOK(status);
        }
        if (status.code() == ErrorCodes::ShardNotFound) {
            shardNotFoundErrorsReceived.emplace_back(shardId, status);
            return;
        }

        if (!firstStaleConfigErrorReceived && ErrorCodes::isStaleShardVersionError(status.code())) {
            firstStaleConfigErrorReceived.emplace(status);
        }

        genericErrorsReceived.emplace_back(shardId, status);
    };

    // Do a pass through all the received responses and group them into success, ShardNotFound, and
    // error responses.
    for (const auto& shardResponse : shardResponses) {
        const auto& shardId = shardResponse.shardId;

        const auto sendStatus = shardResponse.swResponse.getStatus();
        if (!sendStatus.isOK()) {
            processError(shardId, sendStatus);
            continue;
        }

        const auto& resObj = shardResponse.swResponse.getValue().data;
        const auto commandStatus = getStatusFromCommandResult(resObj);
        if (!commandStatus.isOK()) {
            processError(shardId, commandStatus);
            continue;
        }

        if (!firstWriteConcernErrorReceived && resObj["writeConcernError"]) {
            firstWriteConcernErrorReceived.emplace(shardId, resObj["writeConcernError"]);
        }

        successResponsesReceived.emplace_back(shardId, resObj);
        successARSResponses.emplace_back(shardResponse);
        shardsWithSuccessResponses.emplace(shardId);
    }

    // If all shards reported ShardNotFound, promote them all to generic errors.
    if (shardNotFoundErrorsReceived.size() == shardResponses.size()) {
        invariant(genericErrorsReceived.empty());
        genericErrorsReceived = std::move(shardNotFoundErrorsReceived);
    }

    // Append a 'raw' field containing the success responses and error responses.
    BSONObjBuilder rawShardResponses;
    const auto appendRawResponse = [&](const ShardId& shardId, const BSONObj& response) {
        // Try to report the response by the shard's full connection string.
        try {
            const auto shardConnString =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId))
                    ->getConnString();
            rawShardResponses.append(shardConnString.toString(),
                                     CommandHelpers::filterCommandReplyForPassthrough(response));
        } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
            // If we could not get the shard's connection string, fall back to reporting the
            // response by shard id.
            rawShardResponses.append(shardId, response);
        }
    };
    for (const auto& success : successResponsesReceived) {
        appendRawResponse(success.first, success.second);
    }
    for (const auto& error : genericErrorsReceived) {
        BSONObjBuilder statusObjBob;
        CommandHelpers::appendCommandStatusNoThrow(statusObjBob, error.second);
        appendRawResponse(error.first, statusObjBob.obj());
    }
    output->append("raw", rawShardResponses.done());

    // If there were no errors, report success (possibly with a writeConcern error).
    if (genericErrorsReceived.empty()) {
        if (firstWriteConcernErrorReceived) {
            appendWriteConcernErrorToCmdResponse(firstWriteConcernErrorReceived->first,
                                                 firstWriteConcernErrorReceived->second,
                                                 *output);
        }
        return {
            true, shardsWithSuccessResponses, successARSResponses, firstStaleConfigErrorReceived};
    }

    // There was an error. Choose the first error as the top-level error.
    const auto& firstError = genericErrorsReceived.front().second;
    output->append("code", firstError.code());
    output->append("codeName", ErrorCodes::errorString(firstError.code()));
    *errmsg = firstError.reason();
    return {false, shardsWithSuccessResponses, successARSResponses, firstStaleConfigErrorReceived};
}

BSONObj extractQuery(const BSONObj& cmdObj) {
    auto queryElem = cmdObj["query"];
    if (!queryElem)
        queryElem = cmdObj["q"];

    if (!queryElem || queryElem.isNull())
        return BSONObj();

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Illegal type specified for query " << queryElem,
            queryElem.type() == Object);
    return queryElem.embeddedObject();
}

BSONObj extractCollation(const BSONObj& cmdObj) {
    const auto collationElem = cmdObj["collation"];
    if (!collationElem)
        return BSONObj();

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Illegal type specified for collation " << collationElem,
            collationElem.type() == Object);
    return collationElem.embeddedObject();
}

bool appendEmptyResultSet(OperationContext* opCtx,
                          BSONObjBuilder& result,
                          Status status,
                          const std::string& ns) {
    invariant(!status.isOK());

    CurOp::get(opCtx)->debug().nreturned = 0;
    CurOp::get(opCtx)->debug().nShards = 0;

    if (status == ErrorCodes::NamespaceNotFound) {
        // Old style reply
        result << "result" << BSONArray();

        // New (command) style reply
        appendCursorResponseObject(0LL, ns, BSONArray(), &result);

        return true;
    }

    uassertStatusOK(status);
    return true;
}

void createShardDatabase(OperationContext* opCtx, StringData dbName) {
    auto dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);

    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        ConfigsvrCreateDatabase configCreateDatabaseRequest(dbName.toString());
        configCreateDatabaseRequest.setDbName(NamespaceString::kAdminDb);

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto createDbResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(configCreateDatabaseRequest.toBSON({})),
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(createDbResponse.writeConcernStatus);

        if (createDbResponse.commandStatus != ErrorCodes::NamespaceExists) {
            uassertStatusOKWithContext(createDbResponse.commandStatus,
                                       str::stream()
                                           << "Database " << dbName << " could not be created");
        }

        dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
    }

    uassertStatusOKWithContext(dbStatus, str::stream() << "Database " << dbName << " not found");
}

std::set<ShardId> getTargetedShardsForQuery(OperationContext* opCtx,
                                            const CachedCollectionRoutingInfo& routingInfo,
                                            const BSONObj& query,
                                            const BSONObj& collation) {
    if (routingInfo.cm()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation.
        std::set<ShardId> shardIds;
        routingInfo.cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);
        return shardIds;
    }

    // The collection is unsharded. Target only the primary shard for the database.
    return {routingInfo.db().primaryId()};
}

std::vector<std::pair<ShardId, BSONObj>> getVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation) {
    std::vector<std::pair<ShardId, BSONObj>> requests;
    auto ars_requests = buildVersionedRequestsForTargetedShards(
        opCtx, nss, routingInfo, {} /* shardsToSkip */, cmdObj, query, collation);
    std::transform(std::make_move_iterator(ars_requests.begin()),
                   std::make_move_iterator(ars_requests.end()),
                   std::back_inserter(requests),
                   [](auto&& ars) {
                       return std::pair<ShardId, BSONObj>(std::move(ars.shardId),
                                                          std::move(ars.cmdObj));
                   });
    return requests;
}

StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoForTxnCmd(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    invariant(catalogCache);

    // Return the latest routing table if not running in a transaction with snapshot level read
    // concern.
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter || !txnRouter.mustUseAtClusterTime()) {
        return catalogCache->getCollectionRoutingInfo(opCtx, nss);
    }

    auto atClusterTime = txnRouter.getSelectedAtClusterTime();
    return catalogCache->getCollectionRoutingInfoAt(opCtx, nss, atClusterTime.asTimestamp());
}

StatusWith<Shard::QueryResponse> loadIndexesFromAuthoritativeShard(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    auto [indexShard, listIndexesCmd] = [&]() -> std::pair<std::shared_ptr<Shard>, BSONObj> {
        auto cmdNoVersion = applyReadWriteConcern(
            opCtx, true /* appendRC */, false /* appendWC */, BSON("listIndexes" << nss.coll()));
        if (routingInfo.cm()) {
            // For a sharded collection we must load indexes from a shard with chunks. For
            // consistency with cluster listIndexes, load from the shard that owns the minKey chunk.
            const auto minKeyShardId = routingInfo.cm()->getMinKeyShardIdWithSimpleCollation();
            auto minKeyShard =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, minKeyShardId));
            return {minKeyShard,
                    appendShardVersion(cmdNoVersion, routingInfo.cm()->getVersion(minKeyShardId))};
        } else {
            // For an unsharded collection, the primary shard will have correct indexes. We attach
            // unsharded shard version to detect if the collection has become sharded.
            const auto cmdObjWithShardVersion =
                (routingInfo.db().primaryId() != ShardRegistry::kConfigServerShardId)
                ? appendShardVersion(cmdNoVersion, ChunkVersion::UNSHARDED())
                : cmdNoVersion;
            return {routingInfo.db().primary(),
                    appendDbVersionIfPresent(cmdObjWithShardVersion, routingInfo.db())};
        }
    }();

    return indexShard->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        nss.db().toString(),
        listIndexesCmd,
        opCtx->hasDeadline() ? opCtx->getRemainingMaxTimeMillis() : Milliseconds(-1));
}

}  // namespace mongo
