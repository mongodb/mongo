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

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_cluster_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/add_shard_request_type.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

Status notifyShardsOfSecondShardIfNeeded(OperationContext* opCtx) {
    if (!feature_flags::gClusterCardinalityParameter.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return Status::OK();
    }

    auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
    auto* clusterCardinalityParam =
        clusterParameters->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
            "shardedClusterCardinalityForDirectConns");

    auto alreadyHasTwoShards =
        clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();

    // If the cluster already has 2 shards or previously had 2 shards, there is nothing to do.
    if (alreadyHasTwoShards) {
        return Status::OK();
    }

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // If this is only the first shard to be added, there is nothing to do.
    if (shardRegistry->getNumShards(opCtx) < 2) {
        return Status::OK();
    }

    // Set the cluster parameter to disallow direct writes to shards
    ConfigsvrSetClusterParameter configsvrSetClusterParameter(
        BSON("shardedClusterCardinalityForDirectConns" << BSON("hasTwoOrMoreShards" << true)));
    configsvrSetClusterParameter.setDbName(DatabaseName(boost::none, "admin"));

    const auto cmdResponse = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        DatabaseName::kAdmin.toString(),
        configsvrSetClusterParameter.toBSON({}),
        Shard::RetryPolicy::kIdempotent);

    return Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse));
}

}  // namespace

/**
 * Internal sharding command run on config servers to add a shard to the cluster.
 */
class ConfigSvrAddShardCommand : public BasicCommand {
public:
    ConfigSvrAddShardCommand() : BasicCommand("_configsvrAddShard") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and adds a new shard to the cluster.";
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
                "_configsvrAddShard can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto swParsedRequest = AddShardRequest::parseFromConfigCommand(cmdObj);
        uassertStatusOK(swParsedRequest.getStatus());
        auto parsedRequest = std::move(swParsedRequest.getValue());

        auto replCoord = repl::ReplicationCoordinator::get(opCtx);

        auto validationStatus = parsedRequest.validate(replCoord->isConfigLocalHostAllowed());
        uassertStatusOK(validationStatus);

        audit::logAddShard(Client::getCurrent(),
                           parsedRequest.hasName() ? parsedRequest.getName() : "",
                           parsedRequest.getConnString().toString());

        StatusWith<std::string> addShardResult = ShardingCatalogManager::get(opCtx)->addShard(
            opCtx,
            parsedRequest.hasName() ? &parsedRequest.getName() : nullptr,
            parsedRequest.getConnString(),
            false);

        Status status = addShardResult.getStatus();

        if (status.isOK()) {
            status = notifyShardsOfSecondShardIfNeeded(opCtx);
        }

        if (!status.isOK()) {
            LOGV2(21920,
                  "addShard request '{request}' failed: {error}",
                  "addShard request failed",
                  "request"_attr = parsedRequest,
                  "error"_attr = status);
            uassertStatusOK(status);
        }

        result << "shardAdded" << addShardResult.getValue();

        return true;
    }
} configsvrAddShardCmd;

}  // namespace mongo
