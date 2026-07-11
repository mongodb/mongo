// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/checked_cast.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/drop_database_coordinator.h"
#include "mongo/db/global_catalog/ddl/drop_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrDropDatabaseCommand final : public TypedCommand<ShardsvrDropDatabaseCommand> {
public:
    using Request = ShardsvrDropDatabase;

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Drops a database.";
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

            // Since this operation is not directly writing locally we need to force its db
            // profile level increase in order to be logged in "<db>.system.profile"
            CurOp::get(opCtx)->raiseDbProfileLevel(
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel(ns().dbName()));

            const auto requestVersion =
                OperationShardingState::get(opCtx).getDbVersion(ns().dbName());
            const auto service = ShardingCoordinatorService::getService(opCtx);

            const auto dropDatabaseCoordinatorFuture = [&] {
                while (true) {
                    std::shared_ptr<DropDatabaseCoordinator> dropDatabaseCoordinator;

                    {
                        FixedFCVRegion fcvRegion(opCtx);

                        // The Operation FCV is currently propagated only for DDL operations,
                        // which cannot be nested. Therefore, the VersionContext shouldn't have
                        // an OFCV yet.
                        invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());

                        DropDatabaseCoordinatorDocument coordinatorDoc;
                        coordinatorDoc.setShardingCoordinatorMetadata(
                            {{ns(), CoordinatorTypeEnum::kDropDatabase}});

                        dropDatabaseCoordinator = checked_pointer_cast<DropDatabaseCoordinator>(
                            service->getOrCreateInstance(
                                opCtx, coordinatorDoc.toBSON(), fcvRegion));
                    }

                    invariant(dropDatabaseCoordinator);

                    const auto currentDbVersion = dropDatabaseCoordinator->getDatabaseVersion();
                    if (currentDbVersion == requestVersion) {
                        return dropDatabaseCoordinator->getCompletionFuture();
                    }

                    LOGV2_DEBUG(6073000,
                                2,
                                "DbVersion mismatch, waiting for existing coordinator "
                                "to finish",
                                "requestedVersion"_attr = requestVersion,
                                "coordinatorVersion"_attr = currentDbVersion);
                    dropDatabaseCoordinator->getCompletionFuture().wait(opCtx);
                }
            }();

            dropDatabaseCoordinatorFuture.get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
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
MONGO_REGISTER_COMMAND(ShardsvrDropDatabaseCommand).forShard();

}  // namespace
}  // namespace mongo
