/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/remove_shard_commit_coordinator.h"
#include "mongo/s/remove_shard_gen.h"
#include "mongo/s/request_types/remove_shard_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to remove a shard from the cluster.
 *
 * TODO (SERVER-97828): remove this command once the new coordinator is used in the removeShard
 * command.
 */
class ConfigSvrRemoveShardCommitCommand final
    : public TypedCommand<ConfigSvrRemoveShardCommitCommand> {
public:
    using Request = ConfigSvrRemoveShardCommit;
    using Response = RemoveShardResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot run _configsvrRemoveShardCommit when "
                    "featureFlagUseTopologyChangeCoordinators is disabled.",
                    feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrRemoveShard can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto shardId = [&] {
                const auto shardIdOrUrl = request().getCommandParameter();
                const auto shard = uassertStatusOK(
                    Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardIdOrUrl));
                return shard->getId();
            }();

            auto removeShardCommitCoordinator = [&]() {
                auto coordinatorDoc = RemoveShardCommitCoordinatorDocument();
                coordinatorDoc.setShardId(shardId);
                coordinatorDoc.setIsTransitionToDedicated(shardId == ShardId::kConfigServerId);
                coordinatorDoc.setShardingDDLCoordinatorMetadata(
                    {{NamespaceString::kConfigsvrShardsNamespace,
                      DDLCoordinatorTypeEnum::kRemoveShardCommit}});
                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                auto coordinator = checked_pointer_cast<RemoveShardCommitCoordinator>(
                    service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
                return coordinator;
            }();

            const auto& drainingStatus = [&]() -> RemoveShardProgress {
                try {
                    auto drainingStatus = removeShardCommitCoordinator->getResult(opCtx);
                } catch (const ExceptionFor<ErrorCodes::ChunkRangeCleanupPending>&) {
                    return {RemoveShardProgress::PENDING_DATA_CLEANUP,
                            boost::none,
                            topology_change_helpers::getRangeDeletionCount(opCtx)};
                }
                return {RemoveShardProgress::COMPLETED,
                        boost::optional<RemoveShardProgress::DrainingShardUsage>(boost::none)};
            }();

            BSONObjBuilder result;
            ShardingCatalogManager::get(opCtx)->appendShardDrainingStatus(
                opCtx, result, drainingStatus, shardId);

            return Response::parse(IDLParserContext("ConfigsvrRemoveShardCommitCommand"),
                                   result.obj());
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

    bool skipApiVersionCheck() const override {  // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Test command to allow calling the RemoveShardCommitCoordinator while it is "
               "under "
               "development.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};

MONGO_REGISTER_COMMAND(ConfigSvrRemoveShardCommitCommand).testOnly().forShard();

}  // namespace
}  // namespace mongo
