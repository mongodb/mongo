// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/remove_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

/**
 * Internal sharding command run on config servers to get the draining status of a shard from the
 * cluster.
 */
class ConfigSvrShardDrainingStatusCommand
    : public TypedCommand<ConfigSvrShardDrainingStatusCommand> {
public:
    using Request = ConfigSvrShardDrainingStatus;
    using Response = RemoveShardResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrShardDrainingStatus can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Set the operation context read concern level to majority for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

            const auto requestShardId = request().getCommandParameter();
            const auto shardId = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                opCtx, requestShardId, true /* allowNonShardIdIdentifiers */));

            const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);
            const auto isShardDraining =
                shardingCatalogManager->isShardCurrentlyDraining(opCtx, shardId);

            uassert(ErrorCodes::IllegalOperation,
                    fmt::format("The shard {} isn't draining", shardId.toString()),
                    isShardDraining);

            bool isTransitionToDedicatedCS =
                request().getIsTransitionToDedicatedCS().value_or(false);
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot get the draining status for the config server using "
                    "shardDrainingStatus when "
                    "transitioning to a dedicated config server. Please, use "
                    "getTransitionToDedicatedConfigServerStatus.",
                    (isTransitionToDedicatedCS || shardId != ShardId::kConfigServerId));

            const auto shardDrainingState =
                shardingCatalogManager->checkDrainingProgress(opCtx, shardId);

            BSONObjBuilder result;
            shardingCatalogManager->appendShardDrainingStatus(
                opCtx, result, shardDrainingState, shardId.toString());

            Response res = Response::parse(result.obj(), IDLParserContext("removeShardResponse"));
            return res;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Returns the draining status of a specific shard in the cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    };

};  // namespace
MONGO_REGISTER_COMMAND(ConfigSvrShardDrainingStatusCommand).forShard();
}  // namespace mongo
