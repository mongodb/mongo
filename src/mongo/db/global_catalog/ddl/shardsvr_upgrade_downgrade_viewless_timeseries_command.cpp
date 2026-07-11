// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/timeseries_upgrade_downgrade_coordinator.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/topology/sharding_state.h"

namespace mongo {
namespace {

class ShardsvrUpgradeDowngradeViewlessTimeseriesCommand final
    : public TypedCommand<ShardsvrUpgradeDowngradeViewlessTimeseriesCommand> {
public:
    using Request = ShardsvrUpgradeDowngradeViewlessTimeseries;

    std::string help() const override {
        return "Internal command, do not invoke directly. Converts a time-series collection to "
               "viewless/legacy.";
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

            const auto& nss = ns();

            auto coordinatorDoc = TimeseriesUpgradeDowngradeCoordinatorDocument();
            coordinatorDoc.setTimeseriesUpgradeDowngradeRequest(
                request().getTimeseriesUpgradeDowngradeRequest());
            coordinatorDoc.setShardingCoordinatorMetadata(
                {{nss, CoordinatorTypeEnum::kTimeseriesUpgradeDowngrade}});

            auto service = ShardingCoordinatorService::getService(opCtx);
            auto coordinator = checked_pointer_cast<TimeseriesUpgradeDowngradeCoordinator>(
                service->getOrCreateInstance(
                    opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));
            coordinator->getCompletionFuture().get(opCtx);
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
MONGO_REGISTER_COMMAND(ShardsvrUpgradeDowngradeViewlessTimeseriesCommand).forShard();

}  // namespace
}  // namespace mongo
