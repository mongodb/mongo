// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/add_shard_to_zone_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigSvrAddShardToZoneCommand : public TypedCommand<ConfigSvrAddShardToZoneCommand> {
public:
    using Request = ConfigsvrAddShardToZone;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrAddShardToZone can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto resolvedShardId =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                    opCtx,
                    ShardId(std::string{getShard()}),
                    false /* allowNonShardIdIdentifiers */));
            uassertStatusOK(ShardingCatalogManager::get(opCtx)->addShardToZone(
                opCtx, resolvedShardId.toString(), std::string{request().getZone()}));
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        std::string_view getShard() const {
            return request().getCommandParameter();
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and adds a new zone to the shard.";
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrAddShardToZoneCommand).forShard();

}  // namespace
}  // namespace mongo
