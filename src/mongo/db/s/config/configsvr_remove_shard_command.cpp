/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to remove a shard from the cluster.
 */
class ConfigSvrRemoveShardCommand : public BasicCommand {
public:
    ConfigSvrRemoveShardCommand() : BasicCommand("_configsvrRemoveShard") {}

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
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
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
                "_configsvrRemoveShard can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        ON_BLOCK_EXIT([&opCtx] {
            try {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
            } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                // This can throw if the opCtx was interrupted. Catch to prevent crashing.
            }
        });

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        const auto shardId = [&] {
            const auto shardIdOrUrl(cmdObj.firstElement().String());
            auto shard =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardIdOrUrl));
            return shard->getId();
        }();

        uassert(ErrorCodes::IllegalOperation,
                "Cannot remove the config server as a shard using removeShard. To transition the "
                "config shard to a dedicated config server use the "
                "transitionToDedicatedConfigServer command.",
                shardId != ShardId::kConfigServerId);

        const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);

        const auto shardDrainingStatus = [&] {
            try {
                return shardingCatalogManager->removeShard(opCtx, shardId);
            } catch (const DBException& ex) {
                LOGV2(21923,
                      "Failed to remove shard {shardId} due to {error}",
                      "Failed to remove shard",
                      "shardId"_attr = shardId,
                      "error"_attr = redact(ex));
                throw;
            }
        }();

        shardingCatalogManager->appendShardDrainingStatus(
            opCtx, result, shardDrainingStatus, shardId);

        return true;
    }

} configsvrRemoveShardCmd;

}  // namespace
}  // namespace mongo
