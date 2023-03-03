/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers for transitioning from catalog shard to
 * dedicated config server.
 */
class ConfigSvrTransitionToDedicatedConfigCommand : public BasicCommand {
public:
    ConfigSvrTransitionToDedicatedConfigCommand()
        : BasicCommand("_configsvrTransitionToDedicatedConfigServer") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Transitions a cluster to use dedicated config server.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(7368402,
                "The transition to catalog shard feature is disabled",
                gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCV());
        uassert(7467203,
                "The catalog shard feature is disabled",
                gFeatureFlagCatalogShard.isEnabled(serverGlobalParams.featureCompatibility));

        uassert(ErrorCodes::IllegalOperation,
                "_configsvrTransitionToDedicatedConfigServer can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());
        const std::string shardName = shardingState->shardId().toString();

        DBDirectClient client(opCtx);
        BSONObj response;

        // Need to append w: majority because removeShard commands requires it.
        auto removeShardCmd = CommandHelpers::appendMajorityWriteConcern(
            BSON("_configsvrRemoveShard" << shardName), opCtx->getWriteConcern());
        client.runCommand(DatabaseName{"admin"}, removeShardCmd, response);

        uassertStatusOK(getStatusFromCommandResult(response));
        uassertStatusOK(getWriteConcernStatusFromCommandResult(response));

        CommandHelpers::filterCommandReplyForPassthrough(response, &result);

        return true;
    }

} configSvrTransitionToDedicatedConfigCmd;

}  // namespace
}  // namespace mongo
