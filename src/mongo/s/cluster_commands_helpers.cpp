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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/s/cluster_commands_helpers.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
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
#include "mongo/util/log.h"
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

    return stdx::make_unique<WriteConcernErrorDetail>(getWriteConcernErrorDetail(wcErrorElem));
}

namespace {

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo) {
    // Attach the databaseVersion if we have one cached for the database.
    auto dbVersion = dbInfo.databaseVersion();
    if (databaseVersion::isFixed(dbVersion)) {
        return cmdObj;
    }

    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    cmdWithVersionBob.append("databaseVersion", dbVersion.toBSON());
    return cmdWithVersionBob.obj();
}

const auto kAllowImplicitCollectionCreation = "allowImplicitCollectionCreation"_sd;

/**
 * Constructs a requests vector targeting each of the specified shard ids. Each request contains the
 * same cmdObj combined with the default sharding parameters.
 */
std::vector<AsyncRequestsSender::Request> buildUnversionedRequestsForShards(
    OperationContext* opCtx, std::vector<ShardId> shardIds, const BSONObj& cmdObj) {
    auto cmdToSend = cmdObj;
    if (!cmdToSend.hasField(kAllowImplicitCollectionCreation)) {
        cmdToSend = appendAllowImplicitCreate(cmdToSend, false);
    }

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

std::vector<AsyncRequestsSender::Request> buildVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation) {
    if (!routingInfo.cm()) {
        // The collection is unsharded. Target only the primary shard for the database.

        // Attach shardVersion "UNSHARDED", unless targeting the config server.
        const auto cmdObjWithShardVersion = (routingInfo.db().primaryId() != "config")
            ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
            : cmdObj;

        return buildUnversionedRequestsForShards(
            opCtx,
            {routingInfo.db().primaryId()},
            appendDbVersionIfPresent(cmdObjWithShardVersion, routingInfo.db()));
    }

    auto cmdToSend = cmdObj;
    if (!cmdToSend.hasField(kAllowImplicitCollectionCreation)) {
        cmdToSend = appendAllowImplicitCreate(cmdToSend, false);
    }

    std::vector<AsyncRequestsSender::Request> requests;

    // The collection is sharded. Target all shards that own chunks that match the query.
    std::set<ShardId> shardIds;
    routingInfo.cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);

    for (const ShardId& shardId : shardIds) {
        requests.emplace_back(shardId,
                              appendShardVersion(cmdToSend, routingInfo.cm()->getVersion(shardId)));
    }

    return requests;
}

}  // namespace

std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const std::set<ErrorCodes::Error>& ignorableErrors) {

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

            // Failing to establish a consistent shardVersion means no results should be examined.
            if (ErrorCodes::isStaleShardVersionError(status.code())) {
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

            // TODO: This should not be needed once we get better targetting with SERVER-32723.
            // Some commands are sent with allowImplicit: false to all shards and expect only some
            // of them to succeed.
            if (ignorableErrors.find(ErrorCodes::CannotImplicitlyCreateCollection) ==
                    ignorableErrors.end() &&
                ErrorCodes::CannotImplicitlyCreateCollection == status) {
                uassertStatusOK(status);
            }
        }
        responses.push_back(std::move(response));
    }

    return responses;
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
    const auto requests =
        buildVersionedRequestsForTargetedShards(opCtx, nss, routingInfo, cmdObj, query, collation);

    return gatherResponses(opCtx, dbName, readPref, retryPolicy, requests);
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
            opCtx, nss, routingInfo, cmdObj, BSONObj(), BSONObj());
    }

    return gatherResponses(opCtx, nss.db(), readPref, retryPolicy, requests, ignorableErrors);
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

bool appendRawResponses(OperationContext* opCtx,
                        std::string* errmsg,
                        BSONObjBuilder* output,
                        const std::vector<AsyncRequestsSender::Response>& shardResponses,
                        std::set<ErrorCodes::Error> ignorableErrors) {
    // Always include ShardNotFound as an ignorable error, since this node may not have realized a
    // shard has been removed.
    ignorableErrors.insert(ErrorCodes::ShardNotFound);

    std::vector<std::pair<ShardId, BSONObj>> successResponsesReceived;
    std::vector<std::pair<ShardId, Status>> ignorableErrorsReceived;
    std::vector<std::pair<ShardId, Status>> nonIgnorableErrorsReceived;

    boost::optional<std::pair<ShardId, BSONElement>> firstWriteConcernErrorReceived;

    const auto processError = [&](const ShardId& shardId, const Status& status) {
        invariant(!status.isOK());
        if (ignorableErrors.find(status.code()) != ignorableErrors.end()) {
            ignorableErrorsReceived.emplace_back(std::move(shardId), std::move(status));
            return;
        }
        nonIgnorableErrorsReceived.emplace_back(shardId, status);
    };

    // Do a pass through all the received responses and group them into success, ignorable, and
    // non-ignorable.
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
    }

    // If all shards reported ignorable errors, promote them all to non-ignorable errors.
    if (ignorableErrorsReceived.size() == shardResponses.size()) {
        invariant(nonIgnorableErrorsReceived.empty());
        nonIgnorableErrorsReceived = std::move(ignorableErrorsReceived);
    }

    // Append a 'raw' field containing the success responses and non-ignorable error responses.
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
    for (const auto& error : nonIgnorableErrorsReceived) {
        BSONObjBuilder statusObjBob;
        CommandHelpers::appendCommandStatusNoThrow(statusObjBob, error.second);
        appendRawResponse(error.first, statusObjBob.obj());
    }
    output->append("raw", rawShardResponses.done());

    // If there were no non-ignorable errors, report success (possibly with a writeConcern error).
    if (nonIgnorableErrorsReceived.empty()) {
        if (firstWriteConcernErrorReceived) {
            appendWriteConcernErrorToCmdResponse(firstWriteConcernErrorReceived->first,
                                                 firstWriteConcernErrorReceived->second,
                                                 *output);
        }
        return true;
    }

    // There was a non-ignorable error. Choose the first non-ignorable error as the top-level error.
    const auto& firstNonIgnorableError = nonIgnorableErrorsReceived.front().second;
    output->append("code", firstNonIgnorableError.code());
    output->append("codeName", ErrorCodes::errorString(firstNonIgnorableError.code()));
    *errmsg = firstNonIgnorableError.reason();
    return false;
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

int getUniqueCodeFromCommandResults(const std::vector<Strategy::CommandResult>& results) {
    int commonErrCode = -1;
    for (std::vector<Strategy::CommandResult>::const_iterator it = results.begin();
         it != results.end();
         ++it) {
        // Only look at shards with errors.
        if (!it->result["ok"].trueValue()) {
            int errCode = it->result["code"].numberInt();

            if (commonErrCode == -1) {
                commonErrCode = errCode;
            } else if (commonErrCode != errCode) {
                // At least two shards with errors disagree on the error code
                commonErrCode = 0;
            }
        }
    }

    // If no error encountered or shards with errors disagree on the error code, return 0
    if (commonErrCode == -1 || commonErrCode == 0) {
        return 0;
    }

    // Otherwise, shards with errors agree on the error code; return that code
    return commonErrCode;
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
    auto ars_requests =
        buildVersionedRequestsForTargetedShards(opCtx, nss, routingInfo, cmdObj, query, collation);
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

std::vector<AsyncRequestsSender::Response> dispatchCommandAssertCollectionExistsOnAtLeastOneShard(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ReadPreference readPreference,
    const BSONObj& cmdObj) {
    auto shardResponses = scatterGatherOnlyVersionIfUnsharded(
        opCtx,
        nss,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting(readPreference),
        Shard::RetryPolicy::kNoRetry,
        {ErrorCodes::CannotImplicitlyCreateCollection});

    if (std::all_of(shardResponses.begin(), shardResponses.end(), [](const auto& response) {
            return response.swResponse.getStatus().isOK() &&
                getStatusFromCommandResult(response.swResponse.getValue().data) ==
                ErrorCodes::CannotImplicitlyCreateCollection;
        })) {
        // Propagate the ExtraErrorInfo from the first response.
        uassertStatusOK(
            getStatusFromCommandResult(shardResponses.front().swResponse.getValue().data));
    }

    return shardResponses;
}

}  // namespace mongo
