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

void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const ShardCollection& shardCollRequest,
                      const BSONObj& cmdObj,
                      BSONObjBuilder& result) {
    ShardsvrCreateCollection shardsvrCollRequest(nss);
    shardsvrCollRequest.setShardKey(shardCollRequest.getKey());
    shardsvrCollRequest.setUnique(shardCollRequest.getUnique());
    shardsvrCollRequest.setNumInitialChunks(shardCollRequest.getNumInitialChunks());
    shardsvrCollRequest.setPresplitHashedZones(shardCollRequest.getPresplitHashedZones());
    shardsvrCollRequest.setCollation(shardCollRequest.getCollation());
    shardsvrCollRequest.setDbName(nss.db());

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.db()));

    ShardId shardId;
    if (nss.db() == NamespaceString::kConfigDb) {
        const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        uassert(ErrorCodes::IllegalOperation, "there are no shards to target", !shardIds.empty());
        shardId = shardIds[0];
    } else {
        shardId = dbInfo.primaryId();
    }

    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

    auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        nss.db().toString(),
        CommandHelpers::appendMajorityWriteConcern(
            CommandHelpers::appendGenericCommandArgs(cmdObj, shardsvrCollRequest.toBSON({})),
            opCtx->getWriteConcern()),
        Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdResponse.commandStatus);

    CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);

    result.append("collectionsharded", nss.toString());

    auto createCollResp = CreateCollectionResponse::parse(IDLParserErrorContext("createCollection"),
                                                          cmdResponse.response);
    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        nss, createCollResp.getCollectionVersion(), dbInfo.primaryId());
}

void shardCollection(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const ShardCollection& shardCollRequest,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
    ConfigsvrShardCollectionRequest configShardCollRequest;
    configShardCollRequest.set_configsvrShardCollection(nss);
    configShardCollRequest.setKey(shardCollRequest.getKey());
    configShardCollRequest.setUnique(shardCollRequest.getUnique());
    configShardCollRequest.setNumInitialChunks(shardCollRequest.getNumInitialChunks());
    configShardCollRequest.setPresplitHashedZones(shardCollRequest.getPresplitHashedZones());
    configShardCollRequest.setCollation(shardCollRequest.getCollation());

    // Invalidate the routing table cache entry for this collection so that we reload the
    // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
    ON_BLOCK_EXIT([opCtx, nss] {
        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
    });

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        "admin",
        CommandHelpers::appendMajorityWriteConcern(
            CommandHelpers::appendGenericCommandArgs(cmdObj, configShardCollRequest.toBSON()),
            opCtx->getWriteConcern()),
        Shard::RetryPolicy::kIdempotent));

    CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);
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

        if (feature_flags::gShardingFullDDLSupportDistLocksOnStepDown.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            createCollection(opCtx, nss, shardCollRequest, cmdObj, result);
        } else {
            shardCollection(opCtx, nss, shardCollRequest, cmdObj, result);
        }

        return true;
    }

} shardCollectionCmd;

}  // namespace
}  // namespace mongo
