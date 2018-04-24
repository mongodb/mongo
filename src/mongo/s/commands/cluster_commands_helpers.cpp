/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/s/commands/cluster_commands_helpers.h"

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
#include "mongo/s/grid.h"
#include "mongo/s/request_types/create_database_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void appendWriteConcernErrorToCmdResponse(const ShardId& shardId,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder) {
    WriteConcernErrorDetail wcError;
    std::string errMsg;
    auto wcErrorObj = wcErrorElem.Obj();
    if (!wcError.parseBSON(wcErrorObj, &errMsg)) {
        wcError.clear();
        wcError.setStatus({ErrorCodes::FailedToParse,
                           "Failed to parse writeConcernError: " + wcErrorObj.toString() +
                               ", Received error: " + errMsg});
    }
    auto status = wcError.toStatus();
    wcError.setStatus(
        status.withReason(str::stream() << status.reason() << " at " << shardId.toString()));
    responseBuilder.append("writeConcernError", wcError.toBSON());
}

namespace {

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo) {
    // Attach the databaseVersion if we have one cached for the database
    //
    // TODO: After 4.0 is released, require the routingInfo to have a databaseVersion for all
    // databases besides "config" and "admin" (whose primary shard cannot be changed).
    // (In v4.0, if the cluster is in fcv=3.6, we may not have a databaseVersion cached for any
    // database).
    if (!dbInfo.databaseVersion())
        return cmdObj;

    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    cmdWithVersionBob.append("databaseVersion", dbInfo.databaseVersion()->toBSON());
    return cmdWithVersionBob.obj();
}

const auto kAllowImplicitCollectionCreation = "allowImplicitCollectionCreation"_sd;

/**
 * Constructs a requests vector targeting each of the specified shard ids. Each request contains the
 * same cmdObj combined with the default sharding parameters.
 */
std::vector<AsyncRequestsSender::Request> buildUnversionedRequestsForShards(
    std::vector<ShardId> shardIds, const BSONObj& cmdObj) {
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
    return buildUnversionedRequestsForShards(std::move(shardIds), cmdObj);
}

std::vector<AsyncRequestsSender::Request> buildVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
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

/**
 * Throws StaleConfigException if any remote returns a stale shardVersion error.
 */
std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests) {

    // Send the requests.
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            dbName,
                            requests,
                            readPref,
                            retryPolicy);

    // Get the responses.

    std::vector<AsyncRequestsSender::Response> responses;  // Stores results by ShardId
    bool atLeastOneSucceeded = false;
    boost::optional<Status> implicitCreateErrorStatus;

    while (!ars.done()) {
        auto response = ars.next();

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            // Check for special errors that require throwing out any accumulated results.
            auto& responseObj = response.swResponse.getValue().data;
            status = getStatusFromCommandResult(responseObj);

            if (status.isOK()) {
                atLeastOneSucceeded = true;
            }

            // Failing to establish a consistent shardVersion means no results should be examined.
            if (ErrorCodes::isStaleShardVersionError(status.code())) {
                uassertStatusOK(status.withContext(str::stream()
                                                   << "got stale shardVersion response from shard "
                                                   << response.shardId
                                                   << " at host "
                                                   << response.shardHostAndPort->toString()));
            }
            if (ErrorCodes::StaleDbVersion == status) {
                uassertStatusOK(status.withContext(
                    str::stream() << "got stale databaseVersion response from shard "
                                  << response.shardId
                                  << " at host "
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

            if (ErrorCodes::CannotImplicitlyCreateCollection == status) {
                implicitCreateErrorStatus = status;
            }
        }
        responses.push_back(std::move(response));
    }

    // TODO: This should not be needed once we get better targetting with SERVER-32723.
    // Some commands are sent with allowImplicit: false to all shards and expect only some of
    // them to succeed.
    if (implicitCreateErrorStatus && !atLeastOneSucceeded) {
        uassertStatusOK(*implicitCreateErrorStatus);
    }

    return responses;
}

}  // namespace

BSONObj appendShardVersion(BSONObj cmdObj, ChunkVersion version) {
    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    version.appendForCommands(&cmdWithVersionBob);
    return cmdWithVersionBob.obj();
}

BSONObj appendAllowImplicitCreate(BSONObj cmdObj, bool allow) {
    BSONObjBuilder newCmdBuilder(std::move(cmdObj));
    newCmdBuilder.append(kAllowImplicitCollectionCreation, allow);
    return newCmdBuilder.obj();
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
        buildVersionedRequestsForTargetedShards(opCtx, routingInfo, cmdObj, query, collation);

    return gatherResponses(opCtx, dbName, readPref, retryPolicy, requests);
}

std::vector<AsyncRequestsSender::Response> scatterGatherOnlyVersionIfUnsharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
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
            opCtx, routingInfo, cmdObj, BSONObj(), BSONObj());
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
                            {dbInfo.primaryId()}, appendDbVersionIfPresent(cmdObj, dbInfo)));
    return std::move(responses.front());
}

bool appendRawResponses(OperationContext* opCtx,
                        std::string* errmsg,
                        BSONObjBuilder* output,
                        std::vector<AsyncRequestsSender::Response> shardResponses,
                        std::set<ErrorCodes::Error> ignoredErrors) {
    // Always include ShardNotFound as an ignored error, since this node may not have realized a
    // shard has been removed.
    ignoredErrors.insert(ErrorCodes::ShardNotFound);

    BSONObjBuilder subobj;  // Stores raw responses by ConnectionString

    // Stores all errors; we will remove ignoredErrors later if some shard returned success.
    std::vector<std::pair<std::string, Status>> errors;  // Stores errors by ConnectionString

    BSONElement wcErrorElem;  // Stores the first writeConcern error we encounter
    ShardId wcErrorShardId;   // Stores the shardId for the first writeConcern error we encounter
    bool hasWCError = false;  // Whether we have encountered a writeConcern error yet

    for (const auto& shardResponse : shardResponses) {
        // Get the Shard object in order to get the shard's ConnectionString.
        const auto swShard =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardResponse.shardId);
        if (ErrorCodes::ShardNotFound == swShard.getStatus().code()) {
            // Store the error by ShardId, since we cannot know the shard connection string, and it
            // is only used for reporting.
            errors.push_back(std::make_pair(shardResponse.shardId.toString(), swShard.getStatus()));
            continue;
        }
        const auto shard = uassertStatusOK(swShard);
        const auto shardConnStr = shard->getConnString().toString();

        Status sendStatus = shardResponse.swResponse.getStatus();
        if (!sendStatus.isOK()) {
            // Convert the error status back into the form of a command result and append it as the
            // raw response.
            BSONObjBuilder statusObjBob;
            CommandHelpers::appendCommandStatusNoThrow(statusObjBob, sendStatus);
            subobj.append(shardConnStr, statusObjBob.obj());

            errors.push_back(std::make_pair(shardConnStr, sendStatus));
            continue;
        }

        // Got a response from the shard.

        auto& resObj = shardResponse.swResponse.getValue().data;

        // Append the shard's raw response.
        subobj.append(shardConnStr, CommandHelpers::filterCommandReplyForPassthrough(resObj));

        auto commandStatus = getStatusFromCommandResult(resObj);
        if (!commandStatus.isOK()) {
            errors.push_back(std::make_pair(shardConnStr, std::move(commandStatus)));
        }

        // Report the first writeConcern error we see.
        if (!hasWCError && (wcErrorElem = resObj["writeConcernError"])) {
            wcErrorShardId = shardResponse.shardId;
            hasWCError = true;
        }
    }

    output->append("raw", subobj.done());

    if (hasWCError) {
        appendWriteConcernErrorToCmdResponse(wcErrorShardId, wcErrorElem, *output);
    }

    // If any shard returned success, filter out ignored errors
    bool someShardReturnedOK = (errors.size() != shardResponses.size());

    BSONObjBuilder errorBob;
    int commonErrCode = -1;
    auto it = errors.begin();
    while (it != errors.end()) {
        if (someShardReturnedOK && ignoredErrors.find(it->second.code()) != ignoredErrors.end()) {
            // Ignore the error.
            it = errors.erase(it);
        } else {
            errorBob.append(it->first, it->second.reason());
            if (commonErrCode == -1) {
                commonErrCode = it->second.code();
            } else if (commonErrCode != it->second.code()) {
                commonErrCode = 0;
            }
            ++it;
        }
    }
    BSONObj errobj = errorBob.obj();

    if (!errobj.isEmpty()) {
        *errmsg = errobj.toString();

        // If every error has a code, and the code for all errors is the same, then add
        // a top-level field "code" with this value to the output object.
        if (commonErrCode > 0) {
            output->append("code", commonErrCode);
            output->append("codeName", ErrorCodes::errorString(ErrorCodes::Error(commonErrCode)));
            if (errors.size() == 1) {
                // Only propagate extra info if there was a single error object.
                if (auto extraInfo = errors.begin()->second.extraInfo()) {
                    extraInfo->serialize(output);
                }
            }
        }
        return false;
    }
    return true;
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

    return CommandHelpers::appendCommandStatus(result, status);
}

StatusWith<CachedDatabaseInfo> createShardDatabase(OperationContext* opCtx, StringData dbName) {
    auto dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        ConfigsvrCreateDatabase configCreateDatabaseRequest(dbName.toString());
        configCreateDatabaseRequest.setDbName(NamespaceString::kAdminDb);

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto createDbStatus =
            uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                                opCtx,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                "admin",
                                CommandHelpers::appendMajorityWriteConcern(
                                    configCreateDatabaseRequest.toBSON({})),
                                Shard::RetryPolicy::kIdempotent))
                .commandStatus;

        if (createDbStatus.isOK() || createDbStatus == ErrorCodes::NamespaceExists) {
            dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
        } else {
            dbStatus = createDbStatus;
        }
    }

    if (dbStatus.isOK()) {
        return dbStatus;
    }

    return dbStatus.getStatus().withContext(str::stream() << "Database " << dbName << " not found");
}

BSONObj appendAtClusterTime(BSONObj cmdObj, LogicalTime atClusterTime) {
    BSONObjBuilder cmdAtClusterTimeBob;
    for (auto el : cmdObj) {
        if (el.fieldNameStringData() == repl::ReadConcernArgs::kReadConcernFieldName) {
            BSONObjBuilder readConcernBob =
                cmdAtClusterTimeBob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
            for (auto&& elem : el.Obj()) {
                // afterClusterTime cannot be specified with atClusterTime.
                if (elem.fieldNameStringData() !=
                    repl::ReadConcernArgs::kAfterClusterTimeFieldName) {
                    readConcernBob.append(elem);
                }
            }
            readConcernBob.append(repl::ReadConcernArgs::kAtClusterTimeFieldName,
                                  atClusterTime.asTimestamp());
        } else {
            cmdAtClusterTimeBob.append(el);
        }
    }

    return cmdAtClusterTimeBob.obj();
}

BSONObj appendAtClusterTimeToReadConcern(BSONObj readConcernObj, LogicalTime atClusterTime) {
    invariant(readConcernObj[repl::ReadConcernArgs::kAtClusterTimeFieldName].eoo());

    BSONObjBuilder readConcernBob;
    for (auto&& elem : readConcernObj) {
        // afterClusterTime cannot be specified with atClusterTime.
        if (elem.fieldNameStringData() != repl::ReadConcernArgs::kAfterClusterTimeFieldName) {
            readConcernBob.append(elem);
        }
    }

    readConcernBob.append(repl::ReadConcernArgs::kAtClusterTimeFieldName,
                          atClusterTime.asTimestamp());

    return readConcernBob.obj();
}

boost::optional<LogicalTime> computeAtClusterTimeForOneShard(OperationContext* opCtx,
                                                             const ShardId& shardId) {

    if (repl::ReadConcernArgs::get(opCtx).getLevel() !=
        repl::ReadConcernLevel::kSnapshotReadConcern) {
        return boost::none;
    }

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    invariant(shardRegistry);
    return shardRegistry->getShardNoReload(shardId)->getLastCommittedOpTime();
}

namespace {

LogicalTime _computeAtClusterTime(OperationContext* opCtx,
                                  bool mustRunOnAll,
                                  const std::set<ShardId>& shardIds,
                                  const NamespaceString& nss,
                                  const BSONObj query,
                                  const BSONObj collation) {
    // TODO: SERVER-31767
    return LogicalClock::get(opCtx)->getClusterTime();
}

}  // namespace

boost::optional<LogicalTime> computeAtClusterTime(OperationContext* opCtx,
                                                  bool mustRunOnAll,
                                                  const std::set<ShardId>& shardIds,
                                                  const NamespaceString& nss,
                                                  const BSONObj query,
                                                  const BSONObj collation) {

    if (repl::ReadConcernArgs::get(opCtx).getLevel() !=
        repl::ReadConcernLevel::kSnapshotReadConcern) {
        return boost::none;
    }

    auto atClusterTime =
        _computeAtClusterTime(opCtx, mustRunOnAll, shardIds, nss, query, collation);

    // If the user passed afterClusterTime, atClusterTime must be greater than or equal to it.
    const auto afterClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
    if (afterClusterTime && *afterClusterTime > atClusterTime) {
        return afterClusterTime;
    }

    return atClusterTime;
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

}  // namespace mongo
