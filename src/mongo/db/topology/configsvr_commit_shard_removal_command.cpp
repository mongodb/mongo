/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
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
            // which cannot be nested. Therefore, the VersionContext shouldn't have
            // been initialized yet.
            invariant(!VersionContext::getDecoration(opCtx).isInitialized());

            uassert(ErrorCodes::IllegalOperation,
                    "commitShardRemoval is not supported with the current FCV. Upgrade to the "
                    "highest FCV to perform this operation.",
                    feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                        VersionContext::getDecoration(opCtx), (*fixedFCV)->acquireFCVSnapshot()));

            const auto requestShardId = request().getCommandParameter();
            boost::optional<ShardId> shardId;

            auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, requestShardId);

            if (swShard == ErrorCodes::ShardNotFound) {
                return;
            }

            const auto shard = uassertStatusOK(swShard);
            shardId.emplace(shard->getId());

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot remove the config server as a shard using commitShardRemoval. To "
                    "transition the config shard to a dedicated config server use the "
                    "transitionToDedicatedConfigServer command.",
                    *shardId != ShardId::kConfigServerId);

            const auto removeShardResult = topology_change_helpers::runCoordinatorRemoveShard(
                opCtx, ddlLock, fixedFCV, *shardId);

            uassert(ErrorCodes::IllegalOperation,
                    fmt::format("The shard {} isn't completely drained. Please use the "
                                "shardDrainingStatus command to check the draining status",
                                shardId->toString()),
                    removeShardResult.getState() == ShardDrainingStateEnum::kCompleted);
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
