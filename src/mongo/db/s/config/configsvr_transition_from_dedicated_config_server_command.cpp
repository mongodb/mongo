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

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_initialization_mongod.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/add_shard_coordinator.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/transition_from_dedicated_config_server_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ConfigsvrTransitionFromDedicatedConfigServerCommand
    : public TypedCommand<ConfigsvrTransitionFromDedicatedConfigServerCommand> {
public:
    using Request = ConfigsvrTransitionFromDedicatedConfigServer;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Transitions cluster into config shard config servers.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
            uassert(8454803,
                    "The transition to config shard feature is disabled",
                    gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());
            uassert(
                ErrorCodes::IllegalOperation,
                "_configsvrTransitionFromDedicatedConfigServer can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // TODO(SERVER-97816): remove DDL locking and move the fcv upgrade checking logic to the
            // coordinator
            boost::optional<DDLLockManager::ScopedCollectionDDLLock> ddlLock{
                boost::in_place_init,
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "addShard",
                LockMode::MODE_X};
            boost::optional<FixedFCVRegion> fcvRegion{boost::in_place_init, opCtx};
            const auto fcvSnapshot = (*fcvRegion)->acquireFCVSnapshot();

            // (Generic FCV reference): These FCV checks should exist across LTS binary versions.
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Cannot add shard while in upgrading/downgrading FCV state",
                    !fcvSnapshot.isUpgradingOrDowngrading());
            if (feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                    VersionContext::getDecoration(opCtx), fcvSnapshot)) {
                _runNewPath(opCtx, ddlLock, fcvRegion);
            } else {
                _runOldPath(opCtx, *fcvRegion);
            }

            ShardingStatistics::get(opCtx)
                .countTransitionFromDedicatedConfigServerCompleted.addAndFetch(1);
        }

    private:
        void _runOldPath(OperationContext* opCtx, const FixedFCVRegion& fcvRegion) {
            ShardingCatalogManager::get(opCtx)->addConfigShard(opCtx, fcvRegion);
        }

        void _runNewPath(OperationContext* opCtx,
                         boost::optional<DDLLockManager::ScopedCollectionDDLLock>& ddlLock,
                         boost::optional<FixedFCVRegion>& fcvRegion) {
            invariant(ddlLock);
            invariant(fcvRegion);

            const auto [configConnString, shardName] =
                ShardingCatalogManager::get(opCtx)->getConfigShardParameters(opCtx);

            // Since the addShardCoordinator will call functions that will take the FixedFCVRegion
            // the ordering of locks will be DDLLock, FcvLock. We want to maintain this lock
            // ordering to avoid deadlocks. If we only take the FixedFCVRegion before creating the
            // addShardCoordinator, then if it starts to run before we can release the
            // FixedFCVRegion the lock ordering will be reversed (FcvLock, DDLLock). It is safe to
            // take the DDLLock before create the coordinator, as it will only prevent the running
            // of the coordinator while we hold the FixedFCVRegion (FcvLock, DDLLock -> waiting for
            // DDLLock in coordinator). After this we release the locks in reversed order, so we are
            // sure that we are not holding the FixedFCVRegion while we acquire the DDLLock.
            auto coordinator = AddShardCoordinator::create(
                opCtx, *fcvRegion, configConnString, shardName, /*isConfigShard*/ true);

            fcvRegion.reset();
            ddlLock.reset();

            coordinator->getCompletionFuture().get();
        }

        NamespaceString ns() const override {
            return {};
        }

        bool supportsWriteConcern() const override {
            return true;
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
};
MONGO_REGISTER_COMMAND(ConfigsvrTransitionFromDedicatedConfigServerCommand).forShard();

}  // namespace
}  // namespace mongo
