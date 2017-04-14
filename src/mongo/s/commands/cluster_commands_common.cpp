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

#include "mongo/s/commands/cluster_commands_common.h"

#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/version_manager.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
BSONObj appendShardVersion(const BSONObj& cmdObj, ChunkVersion version) {
    BSONObjBuilder cmdWithVersionBob;
    cmdWithVersionBob.appendElements(cmdObj);
    version.appendForCommands(&cmdWithVersionBob);
    return cmdWithVersionBob.obj();
}
}  // namespace

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

std::vector<AsyncRequestsSender::Request> buildRequestsForShardsThatHaveCollection(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj) {
    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // The collection is sharded. Target all shards that own data for the collection.
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
        for (const ShardId& shardId : shardIds) {
            requests.emplace_back(
                shardId, appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId)));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        if (routingInfo.primary()->isConfig()) {
            // Don't append shard version info when contacting the config servers.
            requests.emplace_back(routingInfo.primaryId(), cmdObj);
        } else {
            requests.emplace_back(routingInfo.primaryId(),
                                  appendShardVersion(cmdObj, ChunkVersion::UNSHARDED()));
        }
    }
    return requests;
}

std::vector<AsyncRequestsSender::Request> buildRequestsForShardsForQuery(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& filter,
    const BSONObj& collation) {
    std::vector<AsyncRequestsSender::Request> requests;
    if (routingInfo.cm()) {
        // The collection is sharded. Target all shards that own chunks that match the query.
        std::set<ShardId> shardIds;
        routingInfo.cm()->getShardIdsForQuery(opCtx, filter, collation, &shardIds);
        for (const ShardId& shardId : shardIds) {
            requests.emplace_back(
                shardId, appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId)));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        if (routingInfo.primary()->isConfig()) {
            // Don't append shard version info when contacting the config servers.
            requests.emplace_back(routingInfo.primaryId(), cmdObj);
        } else {
            requests.emplace_back(routingInfo.primaryId(),
                                  appendShardVersion(cmdObj, ChunkVersion::UNSHARDED()));
        }
    }
    return requests;
}


StatusWith<std::vector<AsyncRequestsSender::Response>> gatherResponsesFromShards(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    const std::vector<AsyncRequestsSender::Request>& requests,
    BSONObjBuilder* output,
    BSONObj* viewDefinition) {
    // Extract the readPreference from the command.
    const auto queryOptionsObj = cmdObj.getObjectField(QueryRequest::kUnwrappedReadPrefField);
    const auto readPrefObj = queryOptionsObj.getObjectField(QueryRequest::kWrappedReadPrefField);
    const auto readPref = readPrefObj.isEmpty()
        ? ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet())
        : uassertStatusOK(ReadPreferenceSetting::fromBSON(readPrefObj));

    // Send the requests.
    LOG(0) << "Dispatching command " << redact(cmdObj) << " to " << requests.size()
           << " targeted shards using readPreference " << readPref;
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            dbName,
                            requests,
                            readPref);

    // Get the responses.

    std::vector<AsyncRequestsSender::Response> responses;  // Stores results by ShardId

    // These are only populated if a non-null 'output' was passed, and are used to build 'output'.
    BSONObjBuilder subobj;    // Stores raw responses by ConnectionString
    BSONObjBuilder errors;    // Stores errors by ConnectionString
    int commonErrCode = -1;   // Stores the overall error code
    BSONElement wcErrorElem;  // Stores the first writeConcern error we encounter
    ShardId wcErrorShardId;   // Stores the shardId for the first writeConcern error we encounter
    bool hasWCError = false;  // Whether we have encountered a writeConcern error yet

    ScopeGuard reset = MakeGuard([output]() {
        if (output) {
            output->resetToEmpty();
        }
    });

    while (!ars.done()) {
        auto response = ars.next();
        const auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, response.shardId);
        if (!swShard.isOK()) {
            return swShard.getStatus();
        }
        const auto shard = std::move(swShard.getValue());

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            status = getStatusFromCommandResult(response.swResponse.getValue().data);
            LOG(2) << "Received status " << status << " and responseObj "
                   << response.swResponse.getValue() << " from shard " << response.shardId
                   << " at host " << response.shardHostAndPort->toString();
            // Check for special errors that mean we should retry the entire operation.

            // Failing to establish a consistent shardVersion means the operation should be retried
            // on all shards.
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

            if (output) {
                if (!hasWCError) {
                    if ((wcErrorElem = response.swResponse.getValue().data["writeConcernError"])) {
                        wcErrorShardId = response.shardId;
                        hasWCError = true;
                    }
                }
            }

            if (status.isOK()) {
                // The command status was OK.
                if (output) {
                    subobj.append(shard->getConnString().toString(),
                                  response.swResponse.getValue().data);
                }
                responses.push_back(std::move(response));
                continue;
            }
        }

        // Either we failed to get a response, or the command had a non-OK status that we can store
        // as an individual shard response.

        // Save the extracted command status into the response.
        response.swResponse = status;

        if (output) {
            // Convert the error status back into the format of a command result.
            BSONObjBuilder statusObjBob;
            Command::appendCommandStatus(statusObjBob, status);
            auto statusObj = statusObjBob.obj();

            errors.append(shard->getConnString().toString(), status.reason());
            if (commonErrCode == -1) {
                commonErrCode = status.code();
            } else if (commonErrCode != status.code()) {
                commonErrCode = 0;
            }

            subobj.append(shard->getConnString().toString(), statusObj);
        }
        LOG(2) << "Got error " << response.swResponse.getStatus() << " from shard "
               << response.shardId;
        responses.push_back(std::move(response));
    }

    reset.Dismiss();

    if (output) {
        output->append("raw", subobj.done());

        if (hasWCError) {
            appendWriteConcernErrorToCmdResponse(wcErrorShardId, wcErrorElem, *output);
        }

        BSONObj errobj = errors.done();
        if (!errobj.isEmpty()) {
            // If code for all errors is the same, then report the common error code.
            if (commonErrCode > 0) {
                return {ErrorCodes::fromInt(commonErrCode), errobj.toString()};
            }
            return {ErrorCodes::OperationFailed, errobj.toString()};
        }
    }

    return responses;
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
