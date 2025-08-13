/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/reshard_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/reshard_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(shardsvrReshardCollectionJoinedExistingOperation);

class ShardsvrReshardCollectionCommand final
    : public TypedCommand<ShardsvrReshardCollectionCommand> {
public:
    using Request = ShardsvrReshardCollection;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Reshards a collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            uassert(ErrorCodes::IllegalOperation,
                    "Can't reshard a collection in the config database",
                    !ns().isConfigDB());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());
            resharding::validatePerformVerification(VersionContext::getDecoration(opCtx),
                                                    request().getPerformVerification());

            if (resharding::isMoveCollection(request().getProvenance())) {
                bool clusterHasTwoOrMoreShards = [&]() {
                    auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
                    auto* clusterCardinalityParam =
                        clusterParameters
                            ->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
                                "shardedClusterCardinalityForDirectConns");
                    return clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();
                }();

                uassert(ErrorCodes::IllegalOperation,
                        "Cannot move a collection until a second shard has been successfully added",
                        clusterHasTwoOrMoreShards);

                uassert(ErrorCodes::IllegalOperation,
                        "Can't move an internal resharding collection",
                        !ns().isTemporaryReshardingCollection());

                // TODO (SERVER-88623): re-evalutate the need to track the collection before calling
                // into moveCollection
                ShardsvrCreateCollectionRequest trackCollectionRequest;
                trackCollectionRequest.setUnsplittable(true);
                trackCollectionRequest.setRegisterExistingCollectionInGlobalCatalog(true);
                ShardsvrCreateCollection shardsvrCollCommand(ns());
                shardsvrCollCommand.setShardsvrCreateCollectionRequest(trackCollectionRequest);
                try {
                    cluster::createCollectionWithRouterLoop(opCtx, shardsvrCollCommand);
                } catch (const ExceptionFor<ErrorCodes::LockBusy>& ex) {
                    // If we encounter a lock timeout while trying to track a collection for a
                    // resharding operation, it may indicate that resharding is already running for
                    // this collection. Check if the collection is already tracked and attempt to
                    // join the resharding command if so.
                    try {
                        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, ns());
                    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                        // If the collection isn't tracked, then moveCollection cannot possibly be
                        // ongoing, so we just surface the LockBusy error.
                        throw ex;
                    }
                } catch (const ExceptionFor<ErrorCodes::NamespaceExists>&) {
                    // The registration may throw NamespaceExists when the namespace is a view.
                    // Proceed and let resharding return the proper error in that case.
                }
            }

            const auto reshardCollectionCoordinatorCompletionFuture =
                [&]() -> SharedSemiFuture<void> {
                ReshardCollectionRequest reshardCollectionRequest =
                    request().getReshardCollectionRequest();
                auto coordinatorDoc = ReshardCollectionCoordinatorDocument();
                coordinatorDoc.setReshardCollectionRequest(std::move(reshardCollectionRequest));
                coordinatorDoc.setShardingDDLCoordinatorMetadata(
                    {{ns(), DDLCoordinatorTypeEnum::kReshardCollection}});

                auto service = ShardingDDLCoordinatorService::getService(opCtx);
                auto reshardCollectionCoordinator =
                    checked_pointer_cast<ReshardCollectionCoordinator>(service->getOrCreateInstance(
                        opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));
                return reshardCollectionCoordinator->getCompletionFuture();
            }();

            shardsvrReshardCollectionJoinedExistingOperation.pauseWhileSet();

            reshardCollectionCoordinatorCompletionFuture.get(opCtx);
        }

    private:
        NamespaceString ns() const override {
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
};
MONGO_REGISTER_COMMAND(ShardsvrReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
