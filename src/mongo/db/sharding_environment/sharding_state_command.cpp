// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardingStateCmd : public BasicCommand {
public:
    ShardingStateCmd() : BasicCommand("shardingState") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::shardingState)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const shardingState = ShardingState::get(opCtx);
        const bool isEnabled = shardingState->enabled();
        result.appendBool("enabled", isEnabled);

        if (isEnabled) {
            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
            result.append("configServer",
                          shardRegistry->getConfigServerConnectionString().toString());
            result.append("shardName", shardingState->shardId());
            result.append("clusterId", shardingState->clusterId());

            const auto& shardHandle = shardingState->getShardHandle();
            if (const auto& shardUuid = shardHandle.uuid()) {
                shardUuid->appendToBuilder(&result, "shardUuid");
            }

            CollectionShardingState::appendInfoForShardingStateCommand(opCtx, &result);
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardingStateCmd).forShard();

}  // namespace
}  // namespace mongo
