// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
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
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/remove_shard_command_helpers.h"
#include "mongo/db/topology/remove_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to remove a shard from the cluster.
 */
class ConfigSvrRemoveShardCommand final
    : public BasicCommandWithRequestParser<ConfigSvrRemoveShardCommand> {
public:
    using Request = ConfigSvrRemoveShard;

    ConfigSvrRemoveShardCommand() : BasicCommandWithRequestParser() {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Removes a shard from the cluster.";
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

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName&,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) override {
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        uassert(ErrorCodes::IllegalOperation,
                "_configsvrRemoveShard can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        ON_BLOCK_EXIT([&opCtx] {
            try {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
            } catch (const ExceptionFor<ErrorCategory::Interruption>&) {
                // This can throw if the opCtx was interrupted. Catch to prevent crashing.
            }
        });

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        const auto& request = requestParser.request();
        const auto shardIdOrUrl = request.getCommandParameter();
        boost::optional<ShardId> shardId;

        const auto shardDrainingStatus = [&] {
            try {
                auto swShardId = Grid::get(opCtx)->shardRegistry()->resolveShardId(
                    opCtx, shardIdOrUrl, true /* allowNonShardIdIdentifiers */);
                if (swShardId == ErrorCodes::ShardNotFound) {
                    return RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
                }
                shardId.emplace(uassertStatusOK(swShardId));

                uassert(ErrorCodes::IllegalOperation,
                        "Cannot remove the config server as a shard using removeShard. To "
                        "transition the "
                        "config shard to a dedicated config server use the "
                        "transitionToDedicatedConfigServer command.",
                        *shardId != ShardId::kConfigServerId);

                return topology_change_helpers::removeShard(opCtx, *shardId);
            } catch (const DBException& ex) {
                LOGV2(21923,
                      "Failed to remove shard",
                      "shardId"_attr = shardId ? *shardId : shardIdOrUrl,
                      "error"_attr = redact(ex));
                throw;
            }
        }();

        const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);
        shardingCatalogManager->appendShardDrainingStatus(
            opCtx, result, shardDrainingStatus, shardId ? *shardId : shardIdOrUrl);

        return true;
    }

    void validateResult(const BSONObj& resultObj) final {}
};
MONGO_REGISTER_COMMAND(ConfigSvrRemoveShardCommand).forShard();

}  // namespace
}  // namespace mongo
