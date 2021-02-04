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

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_commands_gen.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

std::vector<AsyncRequestsSender::Request> buildUnshardedRequestsForAllShards(
    OperationContext* opCtx, std::vector<ShardId> shardIds, const BSONObj& cmdObj) {
    auto cmdToSend = cmdObj;
    appendShardVersion(cmdToSend, ChunkVersion::UNSHARDED());

    std::vector<AsyncRequestsSender::Request> requests;
    for (auto&& shardId : shardIds)
        requests.emplace_back(std::move(shardId), cmdToSend);

    return requests;
}

AsyncRequestsSender::Response executeCommandAgainstDatabasePrimaryOrFirstShard(
    OperationContext* opCtx,
    StringData dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    ShardId shardId;
    if (dbName == NamespaceString::kConfigDb) {
        auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        uassert(ErrorCodes::IllegalOperation, "there are no shards to target", !shardIds.empty());
        std::sort(shardIds.begin(), shardIds.end());
        shardId = shardIds[0];
    } else {
        shardId = dbInfo.primaryId();
    }

    auto responses =
        gatherResponses(opCtx,
                        dbName,
                        readPref,
                        retryPolicy,
                        buildUnshardedRequestsForAllShards(
                            opCtx, {shardId}, appendDbVersionIfPresent(cmdObj, dbInfo)));
    return std::move(responses.front());
}

class ShardCollectionCmd : public BasicCommand {
public:
    ShardCollectionCmd() : BasicCommand("shardCollection", "shardcollection") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Shard a collection. Requires key. Optional unique."
               " Sharding must already be enabled for the database.\n"
               "   { enablesharding : \"<dbname>\" }\n";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto shardCollRequest =
            ShardCollection::parse(IDLParserErrorContext("ShardCollection"), cmdObj);

        ShardsvrCreateCollection shardsvrCollRequest(nss);
        shardsvrCollRequest.setShardKey(shardCollRequest.getKey());
        shardsvrCollRequest.setUnique(shardCollRequest.getUnique());
        shardsvrCollRequest.setNumInitialChunks(shardCollRequest.getNumInitialChunks());
        shardsvrCollRequest.setPresplitHashedZones(shardCollRequest.getPresplitHashedZones());
        shardsvrCollRequest.setCollation(shardCollRequest.getCollation());
        shardsvrCollRequest.setDbName(nss.db());

        auto catalogCache = Grid::get(opCtx)->catalogCache();
        const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.db()));

        auto cmdResponse = executeCommandAgainstDatabasePrimaryOrFirstShard(
            opCtx,
            nss.db(),
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendGenericCommandArgs(cmdObj, shardsvrCollRequest.toBSON({})),
                opCtx->getWriteConcern()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);

        const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

        auto createCollResp = CreateCollectionResponse::parse(
            IDLParserErrorContext("createCollection"), remoteResponse.data);

        catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
            nss, createCollResp.getCollectionVersion(), dbInfo.primaryId());

        // Add only collectionsharded as a response parameter and remove the version to maintain the
        // same format as before.
        result.append("collectionsharded", nss.toString());
        auto resultObj =
            remoteResponse.data.removeField(CreateCollectionResponse::kCollectionVersionFieldName);
        CommandHelpers::filterCommandReplyForPassthrough(resultObj, &result);
        return true;
    }

} shardCollectionCmd;

}  // namespace
}  // namespace mongo
