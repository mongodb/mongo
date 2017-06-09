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

std::vector<AsyncRequestsSender::Request> buildRequestsForAllShards(OperationContext* opCtx,
                                                                    const BSONObj& cmdObj) {
    std::vector<AsyncRequestsSender::Request> requests;
    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
    for (auto&& shardId : shardIds) {
        requests.emplace_back(std::move(shardId), cmdObj);
    }
    return requests;
}

std::vector<AsyncRequestsSender::Request> buildRequestsForShardsForNamespace(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const boost::optional<BSONObj> query,
    const boost::optional<BSONObj> collation,
    bool appendVersion) {
    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // The collection is sharded.
        // Note(esha): The for-loop is duplicated because ChunkManager::getShardIdsForQuery() and
        // ShardRegistry::getAllShardIds() return different types: std::set and std::vector,
        // respectively.
        if (query) {
            // A query was specified. Target all shards that own chunks that match the query.
            std::set<ShardId> shardIds;
            routingInfo.cm()->getShardIdsForQuery(
                opCtx, *query, (collation ? *collation : BSONObj()), &shardIds);
            for (const ShardId& shardId : shardIds) {
                requests.emplace_back(
                    shardId,
                    appendVersion
                        ? appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId))
                        : cmdObj);
            }
        } else {
            // No query was specified. Target all shards.
            std::vector<ShardId> shardIds;
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
            for (const ShardId& shardId : shardIds) {
                requests.emplace_back(
                    shardId,
                    appendVersion
                        ? appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId))
                        : cmdObj);
            }
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        requests.emplace_back(routingInfo.primaryId(),
                              appendVersion && !routingInfo.primary()->isConfig()
                                  ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
                                  : cmdObj);
    }
    return requests;
}

StatusWith<std::vector<AsyncRequestsSender::Response>> gatherResponses(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const std::vector<AsyncRequestsSender::Request>& requests,
    BSONObj* viewDefinition) {

    // Send the requests.
    LOG(2) << "Dispatching command " << redact(cmdObj) << " to " << requests.size()
           << " targeted shards using readPreference " << readPref;
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            dbName,
                            requests,
                            readPref);

    // Get the responses.

    std::vector<AsyncRequestsSender::Response> responses;  // Stores results by ShardId

    while (!ars.done()) {
        auto response = ars.next();

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            // Check for special errors that require throwing out any accumulated results.

            status = getStatusFromCommandResult(response.swResponse.getValue().data);
            LOG(2) << "Received status " << status << " and responseObj "
                   << response.swResponse.getValue() << " from shard " << response.shardId
                   << " at host " << response.shardHostAndPort->toString();

            // Failing to establish a consistent shardVersion means no results should be examined.
            if (ErrorCodes::isStaleShardingError(status.code())) {
                return status;
            }

            // In the case a read is performed against a view, the shard primary can return an error
            // indicating that the underlying collection may be sharded. When this occurs the return
            // message will include an expanded view definition and collection namespace. We pass
            // the definition back to the caller by storing it in the 'viewDefinition' parameter.
            // This allows the caller to rewrite the request as an aggregation and retry it.
            if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == status) {
                auto& responseObj = response.swResponse.getValue().data;
                if (!responseObj.hasField("resolvedView")) {
                    status = Status(ErrorCodes::InternalError,
                                    str::stream() << "Missing field 'resolvedView' in document: "
                                                  << responseObj);
                    return status;
                }

                auto resolvedViewObj = responseObj.getObjectField("resolvedView");
                if (resolvedViewObj.isEmpty()) {
                    status = Status(ErrorCodes::InternalError,
                                    str::stream() << "Field 'resolvedView' must be an object: "
                                                  << responseObj);
                    return status;
                }
                if (viewDefinition) {
                    *viewDefinition = BSON("resolvedView" << resolvedViewObj.getOwned());
                }
                return status;
            }

            if (status.isOK()) {
                // The command status was OK.
                responses.push_back(std::move(response));
                continue;
            }
        }

        // Either we failed to get a response, or the command had a non-OK status that we can store
        // as an individual shard response.
        LOG(2) << "Received error " << response.swResponse.getStatus() << " from shard "
               << response.shardId;
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

StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGather(
    OperationContext* opCtx,
    const std::string& dbName,
    const boost::optional<NamespaceString> nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const ShardTargetingPolicy targetPolicy,
    const boost::optional<BSONObj> query,
    const boost::optional<BSONObj> collation,
    const bool appendShardVersion,
    BSONObj* viewDefinition) {

    // If a NamespaceString is specified, it must match the dbName.
    invariant(!nss || (nss.get().db() == dbName));

    switch (targetPolicy) {
        case ShardTargetingPolicy::BroadcastToAllShards: {
            // Send unversioned commands to all shards.
            auto requests = buildRequestsForAllShards(opCtx, cmdObj);
            return gatherResponses(opCtx, dbName, cmdObj, readPref, requests, viewDefinition);
        }

        case ShardTargetingPolicy::UseRoutingTable: {
            // We must have a valid NamespaceString.
            invariant(nss && nss.get().isValid());

            int numAttempts = 0;
            StatusWith<std::vector<AsyncRequestsSender::Response>> swResponses(
                (std::vector<AsyncRequestsSender::Response>()));

            do {
                // Get the routing table cache.
                auto swRoutingInfo =
                    Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, *nss);
                if (!swRoutingInfo.isOK()) {
                    return swRoutingInfo.getStatus();
                }
                auto routingInfo = swRoutingInfo.getValue();

                // Use the routing table cache to decide which shards to target, and build the
                // requests to send to them.
                auto requests = buildRequestsForShardsForNamespace(
                    opCtx, routingInfo, cmdObj, query, collation, appendShardVersion);

                // Retrieve the responses from the shards.
                swResponses =
                    gatherResponses(opCtx, dbName, cmdObj, readPref, requests, viewDefinition);
                ++numAttempts;

                // If any shard returned a stale shardVersion error, invalidate the routing table
                // cache. This will cause the cache to be refreshed the next time it is accessed.
                if (ErrorCodes::isStaleShardingError(swResponses.getStatus().code())) {
                    Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(routingInfo));
                    LOG(1) << "got stale shardVersion error " << swResponses.getStatus()
                           << " while dispatching " << redact(cmdObj) << " after " << numAttempts
                           << " dispatch attempts";
                }
            } while (numAttempts < kMaxNumStaleVersionRetries && !swResponses.getStatus().isOK());

            return swResponses;
        }

        default:
            MONGO_UNREACHABLE;
    }
}

bool appendRawResponses(OperationContext* opCtx,
                        std::string* errmsg,
                        BSONObjBuilder* output,
                        std::vector<AsyncRequestsSender::Response> shardResponses) {
    BSONObjBuilder subobj;    // Stores raw responses by ConnectionString
    BSONObjBuilder errors;    // Stores errors by ConnectionString
    int commonErrCode = -1;   // Stores the overall error code
    BSONElement wcErrorElem;  // Stores the first writeConcern error we encounter
    ShardId wcErrorShardId;   // Stores the shardId for the first writeConcern error we encounter
    bool hasWCError = false;  // Whether we have encountered a writeConcern error yet

    for (const auto& shardResponse : shardResponses) {
        // Get the Shard object in order to get the shard's ConnectionString.
        const auto swShard =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardResponse.shardId);
        if (ErrorCodes::ShardNotFound == swShard.getStatus().code()) {
            // If a shard got removed, ignore its response.
            continue;
        }
        const auto shard = uassertStatusOK(swShard);
        const auto shardConnStr = shard->getConnString().toString();

        auto status = shardResponse.swResponse.getStatus();
        if (status.isOK()) {
            status = getStatusFromCommandResult(shardResponse.swResponse.getValue().data);

            // Report the first writeConcern error we see.
            if (!hasWCError) {
                if ((wcErrorElem = shardResponse.swResponse.getValue().data["writeConcernError"])) {
                    wcErrorShardId = shardResponse.shardId;
                    hasWCError = true;
                }
            }

            if (status.isOK()) {
                subobj.append(shardConnStr, shardResponse.swResponse.getValue().data);
                continue;
            }
        }

        errors.append(shardConnStr, status.reason());

        if (commonErrCode == -1) {
            commonErrCode = status.code();
        } else if (commonErrCode != status.code()) {
            commonErrCode = 0;
        }

        // Convert the error status back into the format of a command result.
        BSONObjBuilder statusObjBob;
        Command::appendCommandStatus(statusObjBob, status);
        subobj.append(shard->getConnString().toString(), statusObjBob.obj());
    }

    output->append("raw", subobj.done());

    if (hasWCError) {
        appendWriteConcernErrorToCmdResponse(wcErrorShardId, wcErrorElem, *output);
    }

    BSONObj errobj = errors.done();
    if (!errobj.isEmpty()) {
        *errmsg = errobj.toString();

        // If every error has a code, and the code for all errors is the same, then add
        // a top-level field "code" with this value to the output object.
        if (commonErrCode > 0) {
            output->append("code", commonErrCode);
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
    uassertStatusOK(Grid::get(opCtx)->catalogClient(opCtx)->getCollections(
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
        auto createDbStatus =
            Grid::get(opCtx)->catalogClient(opCtx)->createDatabase(opCtx, dbName.toString());
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
