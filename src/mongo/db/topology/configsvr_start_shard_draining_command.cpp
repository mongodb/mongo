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
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/remove_shard_command_helpers.h"
#include "mongo/db/topology/remove_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"

#include <string>

#include <boost/move/utility_core.hpp>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

/**
 * Internal sharding command run on config servers to start draining a shard from the cluster.
 */
class ConfigSvrStartShardDrainingCommand : public TypedCommand<ConfigSvrStartShardDrainingCommand> {
public:
    using Request = ConfigSvrStartShardDraining;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrStartShardDraining can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            ON_BLOCK_EXIT([&opCtx] {
                try {
                    repl::ReplClientInfo::forClient(opCtx->getClient())
                        .setLastOpToSystemLastOpTime(opCtx);
                } catch (const ExceptionFor<ErrorCategory::Interruption>&) {
                    // This can throw if the opCtx was interrupted. Catch to prevent crashing.
                }
            });

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto requestShardId = request().getCommandParameter();

            DDLLockManager::ScopedCollectionDDLLock ddlLock(
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "startShardDraining",
                LockMode::MODE_X);

            const auto& shardId = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                opCtx, requestShardId, true /* allowNonShardIdIdentifiers */));
            bool isTransitionToDedicatedCS =
                request().getIsTransitionToDedicatedCS().value_or(false);
            uassert(
                ErrorCodes::IllegalOperation,
                "Cannot start the transition to dedicated config server using "
                "startShardDraining when transitioning to a dedicated config server. Please, use "
                "startTransitionToDedicatedConfigServer.",
                (isTransitionToDedicatedCS || shardId != ShardId::kConfigServerId));

            const auto& progress =
                topology_change_helpers::startShardDraining(opCtx, shardId, ddlLock);

            // The returned progress is empty if the shard is already in draining state, indicating
            // the transition to dedicated config server has already started
            if (isTransitionToDedicatedCS &&
                (progress && progress->getState() == ShardDrainingStateEnum::kStarted)) {
                ShardingStatistics::get(opCtx)
                    .countTransitionToDedicatedConfigServerStarted.addAndFetch(1);
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
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
               "directly. Starts the draining process of a specific shard in the cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    };

};  // namespace
MONGO_REGISTER_COMMAND(ConfigSvrStartShardDrainingCommand).forShard();
}  // namespace mongo
