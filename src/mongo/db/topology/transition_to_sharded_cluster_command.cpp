// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/sharding_environment/sharding_ready.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/transition_to_sharded_cluster_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class TransitionToShardedClusterCommand : public TypedCommand<TransitionToShardedClusterCommand> {
public:
    using Request = TransitionToShardedCluster;

    std::string help() const override {
        return "Transitions a replica set into a sharded cluster.";
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
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(8002900,
                    "Command can only be run on replica sets",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());

            uassert(8002901,
                    "Command cannot be run on dedicated shards",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            if (!ShardingState::get(opCtx)->enabled()) {
                ShardingCatalogManager::get(opCtx)->installConfigShardIdentityDocument(opCtx,
                                                                                       false);
            }

            auto shardingReady = ShardingReady::get(opCtx);
            shardingReady->scheduleTransitionToConfigShard(opCtx);
            shardingReady->waitUntilReady(opCtx);
        }

    private:
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
MONGO_REGISTER_COMMAND(TransitionToShardedClusterCommand).forShard();

}  // namespace
}  // namespace mongo
