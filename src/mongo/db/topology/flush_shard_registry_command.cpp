// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/flush_shard_registry_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class FlushShardRegistryCommand final : public TypedCommand<FlushShardRegistryCommand> {
public:
    using Request = FlushShardRegistry;
    using Response = FlushShardRegistryResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto const grid = Grid::get(opCtx);
            uassert(ErrorCodes::ShardingStateNotInitialized,
                    "Sharding is not initialized",
                    grid->isShardingInitialized());

            auto shardRegistry = grid->shardRegistry();

            BSONObjBuilder registryBobBefore;
            shardRegistry->toBSON(&registryBobBefore);
            BSONObj registryBSONBefore = registryBobBefore.obj();

            LOGV2(11032900,
                  "Forcing refresh of shard registry",
                  "registry"_attr = registryBSONBefore);

            auto [cachedTimeBefore,
                  forceReloadIncrementBefore,
                  cachedTimeAfter,
                  forceReloadIncrementAfter] = shardRegistry->reloadForRecovery(opCtx);

            Response response;
            response.setCachedTimeBefore(cachedTimeBefore);
            response.setForceReloadIncrementBefore(forceReloadIncrementBefore);
            response.setCachedTimeAfter(cachedTimeAfter);
            response.setForceReloadIncrementAfter(forceReloadIncrementAfter);

            BSONObjBuilder registryBobAfter;
            shardRegistry->toBSON(&registryBobAfter);
            BSONObj registryBSONAfter = registryBobAfter.obj();

            LOGV2(
                11032901, "Shard registry refresh completed", "registry"_attr = registryBSONAfter);

            return response;
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
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

    std::string help() const override {
        return "Internal command to flush the shard registry on the local node. This command is "
               "used to clear the cached connection strings and ReplicaSetMonitors.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(FlushShardRegistryCommand).forRouter().forShard();

}  // namespace
}  // namespace mongo
