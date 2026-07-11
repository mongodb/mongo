// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrDropCollectionCommand final : public TypedCommand<ShardsvrDropCollectionCommand> {
public:
    using Request = ShardsvrDropCollection;

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Drops a collection.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            try {
                const auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, ns());

                uassert(ErrorCodes::IllegalOperation,
                        "Sharded time-series buckets collections cannot be dropped directly; drop "
                        "the logical namespace instead",
                        !coll.getTimeseriesFields() || !ns().isTimeseriesBucketsCollection() ||
                            coll.getUnsplittable());

            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // The collection is not tracked or doesn't exist.
            }

            // Since this operation is not directly writing locally we need to force its db
            // profile level increase in order to be logged in "<db>.system.profile"
            CurOp::get(opCtx)->raiseDbProfileLevel(
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel(ns().dbName()));

            auto coordinatorDoc = DropCollectionCoordinatorDocument();
            coordinatorDoc.setShardingCoordinatorMetadata(
                {{ns(), CoordinatorTypeEnum::kDropCollection}});
            coordinatorDoc.setCollectionUUID(request().getCollectionUUID());

            auto service = ShardingCoordinatorService::getService(opCtx);
            auto dropCollCoordinator =
                checked_pointer_cast<DropCollectionCoordinator>(service->getOrCreateInstance(
                    opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));

            dropCollCoordinator->getCompletionFuture().get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
MONGO_REGISTER_COMMAND(ShardsvrDropCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
