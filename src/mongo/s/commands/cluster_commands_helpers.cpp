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

#include "mongo/s/commands/cluster_commands_helpers.h"

#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/version_manager.h"
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
        wcError.setErrMessage("Failed to parse writeConcernError: " + wcErrorObj.toString() +
                              ", Received error: " + errMsg);
    }
    wcError.setErrMessage(wcError.getErrMessage() + " at " + shardId.toString());
    responseBuilder.append("writeConcernError", wcError.toBSON());
}

namespace {

std::vector<AsyncRequestsSender::Request> buildUnversionedRequestsForAllShards(
    OperationContext* opCtx, const BSONObj& cmdObj) {
    std::vector<AsyncRequestsSender::Request> requests;
    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
    for (auto&& shardId : shardIds) {
        requests.emplace_back(std::move(shardId), cmdObj);
    }
    return requests;
}

std::vector<AsyncRequestsSender::Request> buildVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation) {
    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // The collection is sharded. Target all shards that own chunks that match the query.
        std::set<ShardId> shardIds;
        routingInfo.cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);
        for (const ShardId& shardId : shardIds) {
            requests.emplace_back(
                shardId, appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId)));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        requests.emplace_back(routingInfo.primaryId(),
                              !routingInfo.primary()->isConfig()
                                  ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
                                  : cmdObj);
    }
    return requests;
}

/**
 * Throws StaleConfigException if any remote returns a stale shardVersion error.
 */
StatusWith<std::vector<AsyncRequestsSender::Response>> gatherResponses(
    OperationContext* opCtx,
    const std::string& dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    BSONObj* viewDefinition) {

    // Send the requests.
    AsyncRequestsSender ars(opCtx,
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
            if (ErrorCodes::isStaleShardingError(status.code())) {
                throw StaleConfigException(str::stream() << "got stale shardVersion response "
                                                         << responseObj
                                                         << " from shard "
                                                         << response.shardId
                                                         << " at host "
                                                         << response.shardHostAndPort->toString(),
                                           responseObj);
            }

            // In the case a read is performed against a view, the server can return an error
            // indicating that the underlying collection may be sharded. When this occurs the return
            // message will include an expanded view definition and collection namespace. We pass
            // the definition back to the caller by storing it in the 'viewDefinition' parameter.
            // This allows the caller to rewrite the request as an aggregation and retry it.
            if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == status) {
                if (!responseObj.hasField("resolvedView")) {
                    return {ErrorCodes::InternalError,
                            str::stream() << "Missing field 'resolvedView' in document: "
                                          << responseObj};
                }

                auto resolvedViewObj = responseObj.getObjectField("resolvedView");
                if (resolvedViewObj.isEmpty()) {
                    return {ErrorCodes::InternalError,
                            str::stream() << "Field 'resolvedView' must be an object: "
                                          << responseObj};
                }

                if (viewDefinition) {
                    *viewDefinition = BSON("resolvedView" << resolvedViewObj.getOwned());
                }
                return status;
            }
        }
        responses.push_back(std::move(response));
    }
    return responses;
}

}  // namespace

BSONObj appendShardVersion(BSONObj cmdObj, ChunkVersion version) {
    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    version.appendForCommands(&cmdWithVersionBob);
    return cmdWithVersionBob.obj();
}

StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherUnversionedTargetAllShards(
    OperationContext* opCtx,
    const std::string& dbName,
    boost::optional<NamespaceString> nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    // Some commands, such as $currentOp, operate on a collectionless namespace. If a full namespace
    // is specified, its database must match the dbName.
    invariant(!nss || (nss->db() == dbName));

    auto requests = buildUnversionedRequestsForAllShards(opCtx, cmdObj);

    return gatherResponses(
        opCtx, dbName, readPref, retryPolicy, requests, nullptr /* viewDefinition */);
}

StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    const std::string& dbName,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    BSONObj* viewDefinition) {
    // The database in the full namespace must match the dbName.
    invariant(nss.db() == dbName);

    auto swRoutingInfo = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (!swRoutingInfo.isOK()) {
        return swRoutingInfo.getStatus();
    }
    auto routingInfo = swRoutingInfo.getValue();

    auto requests =
        buildVersionedRequestsForTargetedShards(opCtx, routingInfo, cmdObj, query, collation);

    return gatherResponses(opCtx, dbName, readPref, retryPolicy, requests, viewDefinition);
}

StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherOnlyVersionIfUnsharded(
    OperationContext* opCtx,
    const std::string& dbName,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    // The database in the full namespace must match the dbName.
    invariant(nss.db() == dbName);

    auto swRoutingInfo = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (!swRoutingInfo.isOK()) {
        return swRoutingInfo.getStatus();
    }
    auto routingInfo = swRoutingInfo.getValue();

    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // An unversioned request on a sharded collection can cause a shard that has not owned data
        // for the collection yet to implicitly create the collection without all the collection
        // options. So, we signal to shards that they should not implicitly create the collection.
        BSONObjBuilder augmentedCmdBob;
        augmentedCmdBob.appendElementsUnique(cmdObj);
        augmentedCmdBob.append("allowImplicitCollectionCreation", false);
        requests = buildUnversionedRequestsForAllShards(opCtx, augmentedCmdBob.obj());
    } else {
        requests = buildVersionedRequestsForTargetedShards(
            opCtx, routingInfo, cmdObj, BSONObj(), BSONObj());
    }

    return gatherResponses(
        opCtx, dbName, readPref, retryPolicy, requests, nullptr /* viewDefinition */);
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
            Command::appendCommandStatus(statusObjBob, sendStatus);
            subobj.append(shardConnStr, statusObjBob.obj());

            errors.push_back(std::make_pair(shardConnStr, sendStatus));
            continue;
        }

        // Got a response from the shard.

        auto& resObj = shardResponse.swResponse.getValue().data;

        // Append the shard's raw response.
        subobj.append(shardConnStr, Command::filterCommandReplyForPassthrough(resObj));

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
        }
        return false;
    }
    return true;
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

bool appendEmptyResultSet(BSONObjBuilder& result, Status status, const std::string& ns) {
    invariant(!status.isOK());

    if (status == ErrorCodes::NamespaceNotFound) {
        // Old style reply
        result << "result" << BSONArray();

        // New (command) style reply
        appendCursorResponseObject(0LL, ns, BSONArray(), &result);

        return true;
    }

    return Command::appendCommandStatus(result, status);
}

std::vector<NamespaceString> getAllShardedCollectionsForDb(OperationContext* opCtx,
                                                           StringData dbName) {
    const auto dbNameStr = dbName.toString();

    std::vector<CollectionType> collectionsOnConfig;
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollections(
        opCtx, &dbNameStr, &collectionsOnConfig, nullptr));

    std::vector<NamespaceString> collectionsToReturn;
    for (const auto& coll : collectionsOnConfig) {
        if (coll.getDropped())
            continue;

        collectionsToReturn.push_back(coll.getNs());
    }

    return collectionsToReturn;
}

CachedCollectionRoutingInfo getShardedCollection(OperationContext* opCtx,
                                                 const NamespaceString& nss) {
    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection " << nss.ns() << " is not sharded.",
            routingInfo.cm());

    return routingInfo;
}

StatusWith<CachedDatabaseInfo> createShardDatabase(OperationContext* opCtx, StringData dbName) {
    auto dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        ConfigsvrCreateDatabase configCreateDatabaseRequest;
        configCreateDatabaseRequest.set_configsvrCreateDatabase(dbName);

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto createDbStatus =
            uassertStatusOK(
                configShard->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    "admin",
                    Command::appendMajorityWriteConcern(configCreateDatabaseRequest.toBSON()),
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

    return {dbStatus.getStatus().code(),
            str::stream() << "Database " << dbName << " not found due to "
                          << dbStatus.getStatus().reason()};
}

}  // namespace mongo
