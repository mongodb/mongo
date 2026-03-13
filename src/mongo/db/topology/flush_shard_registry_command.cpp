/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
