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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/remove_shard_command_helpers.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/transition_to_dedicated_config_server_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"

#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers for transitioning from config shard to
 * dedicated config server.
 */
class ConfigsvrTransitionToDedicatedConfigCommand
    : public TypedCommand<ConfigsvrTransitionToDedicatedConfigCommand> {
public:
    using Request = ConfigsvrTransitionToDedicatedConfig;
    using Response = RemoveShardResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
            uassert(7368402,
                    "The transition to config shard feature is disabled",
                    gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe());

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrTransitionToDedicatedConfigServer can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            CommandHelpers::uassertCommandRunWithMajority(
                ConfigsvrTransitionToDedicatedConfig::kCommandName, opCtx->getWriteConcern());
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

            auto shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();
            const auto shardId = shardingState->shardId();

            const auto shardDrainingStatus = [&] {
                try {
                    const auto swShard =
                        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
                    if (swShard == ErrorCodes::ShardNotFound) {
                        return RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
                    }
                    const auto shard = uassertStatusOK(swShard);

                    return topology_change_helpers::removeShard(opCtx, shard->getId());
                } catch (const DBException& ex) {
                    LOGV2(7470500,
                          "Failed to remove shard",
                          "shardId"_attr = shardId,
                          "error"_attr = redact(ex));
                    throw;
                }
            }();

            const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);
            BSONObjBuilder result;
            shardingCatalogManager->appendShardDrainingStatus(
                opCtx, result, shardDrainingStatus, shardId);

            if (shardDrainingStatus.getState() == ShardDrainingStateEnum::kCompleted) {
                ShardingStatistics::get(opCtx)
                    .countTransitionToDedicatedConfigServerCompleted.addAndFetch(1);
            } else if (shardDrainingStatus.getState() == ShardDrainingStateEnum::kStarted) {
                ShardingStatistics::get(opCtx)
                    .countTransitionToDedicatedConfigServerStarted.addAndFetch(1);
            }

            return Response::parse(result.obj(),
                                   IDLParserContext("ConfigsvrTransitionToDedicatedConfigCommand"));
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

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Transitions a cluster to use dedicated config server.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrTransitionToDedicatedConfigCommand).forShard();

}  // namespace
}  // namespace mongo
