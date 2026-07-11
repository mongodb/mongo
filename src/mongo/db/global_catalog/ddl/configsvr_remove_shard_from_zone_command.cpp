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
#include "mongo/db/global_catalog/ddl/remove_shard_from_zone_request_type.h"
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

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using std::string;

/**
 * Internal sharding command run on config servers to remove a shard from the given zone.
 *
 * Format:
 * {
 *   _configsvrRemoveShardFromZone: <string shardName>,
 *   zone: <string zoneName>,
 *   writeConcern: <BSONObj>
 * }
 */
class ConfigSvrRemoveShardFromZoneCommand : public BasicCommand {
public:
    ConfigSvrRemoveShardFromZoneCommand() : BasicCommand("_configsvrRemoveShardFromZone") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and removes the shard from the zone.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrRemoveShardFromZone can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto parsedRequest =
            uassertStatusOK(RemoveShardFromZoneRequest::parseFromConfigCommand(cmdObj));

        const auto resolvedShardId =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
                opCtx,
                ShardId(parsedRequest.getShardName()),
                false /* allowNonShardIdIdentifiers */));
        uassertStatusOK(ShardingCatalogManager::get(opCtx)->removeShardFromZone(
            opCtx, resolvedShardId.toString(), parsedRequest.getZoneName()));

        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrRemoveShardFromZoneCommand).forShard();

}  // namespace
}  // namespace mongo
