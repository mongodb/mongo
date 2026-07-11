// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/cluster_role.h"
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
 * Internal sharding command run on config servers to remove a shard from the cluster.
 */
class ConfigSvrCommitShardRemovalCommand : public TypedCommand<ConfigSvrCommitShardRemovalCommand> {
public:
    using Request = ConfigSvrCommitShardRemoval;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitShardRemoval can only be run on config servers",
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

            boost::optional<DDLLockManager::ScopedCollectionDDLLock> ddlLock{
                boost::in_place_init,
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "commitShardRemoval",
                LockMode::MODE_X};

            boost::optional<FixedFCVRegion> fixedFCV{boost::in_place_init, opCtx};
            // The Operation FCV is currently propagated only for DDL operations,
            // which cannot be nested. Therefore, the VersionContext shouldn't have an OFCV yet.
            invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());

            uassert(ErrorCodes::IllegalOperation,
                    "commitShardRemoval is not supported with the current FCV. Upgrade to the "
                    "highest FCV to perform this operation.",
                    feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                        VersionContext::getDecoration(opCtx), (*fixedFCV)->acquireFCVSnapshot()));

            const auto requestShardId = request().getCommandParameter();
            auto swShardId = Grid::get(opCtx)->shardRegistry()->resolveShardId(
                opCtx, requestShardId, true /* allowNonShardIdIdentifiers */);
            if (swShardId == ErrorCodes::ShardNotFound) {
                return;
            }
            const auto& shardId = uassertStatusOK(swShardId);

            bool isTransitionToDedicatedCS =
                request().getIsTransitionToDedicatedCS().value_or(false);
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot remove the config server as a shard using commitShardRemoval when "
                    "transitioning to a dedicated config server. Please, use "
                    "commitTransitionToDedicatedConfigServer to complete the transition to "
                    "dedicated config server.",
                    (isTransitionToDedicatedCS || shardId != ShardId::kConfigServerId));

            const auto removeShardResult = topology_change_helpers::runCoordinatorRemoveShard(
                opCtx, ddlLock, fixedFCV, shardId);

            uassert(ErrorCodes::IllegalOperation,
                    fmt::format("The shard {} isn't completely drained. Please use the {} command "
                                "to check the draining status.",
                                shardId.toString(),
                                isTransitionToDedicatedCS
                                    ? "getTransitionToDedicatedConfigServerStatus"
                                    : "shardDrainingStatus"),
                    removeShardResult.getState() == ShardDrainingStateEnum::kCompleted);

            if (isTransitionToDedicatedCS) {
                ShardingStatistics::get(opCtx)
                    .countTransitionToDedicatedConfigServerCompleted.addAndFetch(1);
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
               "directly. Removes a specific shard from the cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    };

};  // namespace
MONGO_REGISTER_COMMAND(ConfigSvrCommitShardRemovalCommand).forShard();
}  // namespace mongo
